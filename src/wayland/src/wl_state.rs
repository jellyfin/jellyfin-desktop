//! Wayland surface / present / transition state.
//!
//! Dispatches CEF-typed structs unpacked to plain integers into the
//! FFI entry points exposed by [`crate::wl_ffi`].
//!
//! Owns:
//!   * A dedicated `EventQueue` over an mpv-owned `wl_display`
//!     (foreign-display backend, never closes the fd)
//!   * Bindings for `wl_compositor`, `wl_subcompositor`, `wl_shm`,
//!     `zwp_linux_dmabuf_v1`, `wp_viewporter`
//!   * The list of per-layer `PlatformSurface`s and their popup
//!     children
//!   * The fullscreen-transition state machine (begin/end + tolerance
//!     gate for the paint path)
//!
//! All mutable state lives behind a single `Mutex` — mirrors the C++
//! `surface_mtx` discipline. Coarse locking is intentional: the paint
//! path holds the lock during commit/flush, and finer-grained locking
//! would risk null-attach vs. commit ordering races.

use parking_lot::{Mutex, MutexGuard};
use std::ffi::c_void;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, FromRawFd, OwnedFd};
use std::ptr::NonNull;
use std::sync::{Arc, OnceLock};

use jfn_gpu_paint::GpuContext;

use crate::gpu_paint_worker::WaylandGpuPaintWorker;
use crate::shm_paint_worker::WaylandShmPaintWorker;

use memmap2::MmapOptions;
use wayland_backend::client::{Backend, ObjectId};
use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::{
    wl_buffer::WlBuffer,
    wl_compositor::WlCompositor,
    wl_region::WlRegion,
    wl_registry::WlRegistry,
    wl_shm::{Format, WlShm},
    wl_shm_pool::WlShmPool,
    wl_subcompositor::WlSubcompositor,
    wl_subsurface::WlSubsurface,
    wl_surface::WlSurface,
};
use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle};
use wayland_protocols::wp::linux_dmabuf::zv1::client::{
    zwp_linux_buffer_params_v1::{Flags as DmabufFlags, ZwpLinuxBufferParamsV1},
    zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1,
};
use wayland_protocols::wp::viewporter::client::{
    wp_viewport::WpViewport, wp_viewporter::WpViewporter,
};

const fn fourcc(a: u8, b: u8, c: u8, d: u8) -> u32 {
    (a as u32) | ((b as u32) << 8) | ((c as u32) << 16) | ((d as u32) << 24)
}

const DRM_FORMAT_ARGB8888: u32 = fourcc(b'A', b'R', b'2', b'4');

/// FS transition tolerance in texels — first paint within this of the
/// new mpv size ends the transition.
pub(crate) const TRANSITION_TOLERANCE_TEXELS: i32 = 32;

// =====================================================================
// Per-surface state
// =====================================================================

/// Per-CefLayer surface. Owns its subsurface, viewport
/// proxies, and its on-demand popup child.
pub(crate) struct PlatformSurface {
    pub surface: Option<WlSurface>,
    pub subsurface: Option<WlSubsurface>,
    pub viewport: Option<WpViewport>,
    pub buffer: Option<WlBuffer>,
    pub buffer_w: i32,
    pub buffer_h: i32,
    pub visible: bool,
    pub placeholder: bool,
    pub null_attached: bool,
    pub lw: i32,
    pub lh: i32,
    pub pw: i32,
    pub ph: i32,

    // Popup child.
    pub popup_surface: Option<WlSurface>,
    pub popup_subsurface: Option<WlSubsurface>,
    pub popup_viewport: Option<WpViewport>,
    pub popup_buffer: Option<WlBuffer>,
    pub popup_visible: bool,

    /// Vulkan-WSI presenter worker, lazily created on first software
    /// present when `WlState::use_gpu_paint` is set. The worker owns the
    /// per-surface GpuPainter/swapchain so CEF paint callbacks only copy
    /// latest pixels and signal it.
    pub gpu_paint_worker: Option<WaylandGpuPaintWorker>,
    pub shm_paint_worker: Option<WaylandShmPaintWorker>,
}

impl PlatformSurface {
    pub(crate) fn new() -> Self {
        Self {
            surface: None,
            subsurface: None,
            viewport: None,
            buffer: None,
            buffer_w: 0,
            buffer_h: 0,
            visible: true,
            placeholder: false,
            null_attached: false,
            lw: 0,
            lh: 0,
            pw: 0,
            ph: 0,
            popup_surface: None,
            popup_subsurface: None,
            popup_viewport: None,
            popup_buffer: None,
            popup_visible: false,
            gpu_paint_worker: None,
            shm_paint_worker: None,
        }
    }
}

// =====================================================================
// Paint path discriminator (replaces the C++ g_present function pointer)
// =====================================================================

#[derive(Copy, Clone, Eq, PartialEq)]
pub(crate) enum PresentMode {
    /// Steady-state: attach the dmabuf provided.
    Attach,
    /// Drop frames silently — used between begin_transition and the
    /// first on_mpv_configure that publishes the new size.
    Drop,
}

// =====================================================================
// Wl-side state (one global, mutex-guarded — mirrors C++ surface_mtx)
// =====================================================================

pub(crate) struct WlState {
    pub conn: Connection,
    pub qh: QueueHandle<DispatchState>,
    /// Dedicated event queue — kept alive so all our proxies route here
    /// instead of mpv's default queue.
    #[allow(dead_code)]
    pub queue: EventQueue<DispatchState>,

    pub compositor: WlCompositor,
    pub subcompositor: WlSubcompositor,
    pub shm: WlShm,
    pub dmabuf: Option<ZwpLinuxDmabufV1>,
    pub viewporter: Option<WpViewporter>,

    /// mpv-owned parent surface (foreign object — never destroyed by us).
    pub parent: WlSurface,

    /// Stack order, bottom-to-top. Raw pointers are valid for the
    /// lifetime of each `PlatformSurface` (heap-allocated via `Box`,
    /// removed before drop).
    pub stack: Vec<*mut PlatformSurface>,

    pub was_fullscreen: bool,
    pub transitioning: bool,
    pub present_mode: PresentMode,

    /// Raw `wl_display*` — kept so `GpuPainter::new` can build
    /// `VK_KHR_wayland_surface` handles for child surfaces.
    pub display_ptr: NonNull<c_void>,
    /// Shared Vulkan context. `None` outside `--wayland-paint=gpu` and
    /// outside the EGL-fail-with-Vulkan-adapter fallback path.
    pub gpu_ctx: Option<Arc<GpuContext>>,
    /// When true, `surface_present_software` routes through each
    /// surface's GPU paint worker (Vulkan WSI) instead of `wl_shm`.
    /// `set_visible` and `resize` also skip their
    /// `wl_surface.attach`/`viewport.set_destination` work for the
    /// gpu_paint surface.
    pub use_gpu_paint: bool,
}

// Raw pointers in `stack` are only ever dereferenced under the Mutex
// that wraps the WlState itself.
unsafe impl Send for WlState {}

/// Zero-state Dispatch sink — we ignore all protocol events.
pub(crate) struct DispatchState;

static STATE: OnceLock<Mutex<WlState>> = OnceLock::new();

pub(crate) fn try_state() -> Option<&'static Mutex<WlState>> {
    STATE.get()
}

pub(crate) fn lock() -> MutexGuard<'static, WlState> {
    STATE.get().expect("wl_state used before init").lock()
}

// =====================================================================
// Dispatch impls — all no-ops; events we'd care about (wl_buffer.release,
// dmabuf format/modifier) are intentionally ignored to match the C++
// implementation's behavior.
// =====================================================================

impl Dispatch<WlRegistry, GlobalListContents> for DispatchState {
    fn event(
        _: &mut Self,
        _: &WlRegistry,
        _: <WlRegistry as Proxy>::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

macro_rules! noop_dispatch {
    ($($ty:ty),+ $(,)?) => {
        $(
            impl Dispatch<$ty, ()> for DispatchState {
                fn event(
                    _: &mut Self,
                    _: &$ty,
                    _: <$ty as Proxy>::Event,
                    _: &(),
                    _: &Connection,
                    _: &QueueHandle<Self>,
                ) {}
            }
        )+
    };
}

noop_dispatch!(
    WlCompositor,
    WlSubcompositor,
    WlSurface,
    WlSubsurface,
    WlRegion,
    WlShm,
    WlShmPool,
    WlBuffer,
    ZwpLinuxDmabufV1,
    ZwpLinuxBufferParamsV1,
    WpViewporter,
    WpViewport,
);

// =====================================================================
// Init — bind globals against a dedicated EventQueue over the foreign
// (mpv-owned) wl_display. The parent surface is wrapped via
// ObjectId::from_ptr; it stays unmanaged on our side so destruction
// remains mpv's responsibility.
// =====================================================================

/// SAFETY: `display_ptr` must be a live `*mut wl_display` owned by an
/// external party (mpv); `parent_surface_ptr` must be a live
/// `*mut wl_proxy` referring to a `wl_surface` on that display.
pub(crate) unsafe fn init(
    display_ptr: *mut c_void,
    parent_surface_ptr: *mut c_void,
) -> Result<(), String> {
    if STATE.get().is_some() {
        return Err("wl_state already initialised".into());
    }
    if display_ptr.is_null() || parent_surface_ptr.is_null() {
        return Err("null display or parent surface".into());
    }

    let backend = unsafe { Backend::from_foreign_display(display_ptr.cast()) };
    let conn = Connection::from_backend(backend);
    let (globals, queue) =
        registry_queue_init::<DispatchState>(&conn).map_err(|e| format!("registry init: {e}"))?;
    let qh = queue.handle();

    let compositor: WlCompositor = globals
        .bind(&qh, 1..=4, ())
        .map_err(|e| format!("bind wl_compositor: {e}"))?;
    let subcompositor: WlSubcompositor = globals
        .bind(&qh, 1..=1, ())
        .map_err(|e| format!("bind wl_subcompositor: {e}"))?;
    let shm: WlShm = globals
        .bind(&qh, 1..=1, ())
        .map_err(|e| format!("bind wl_shm: {e}"))?;
    let dmabuf: Option<ZwpLinuxDmabufV1> = globals.bind(&qh, 1..=4, ()).ok();
    let viewporter: Option<WpViewporter> = globals.bind(&qh, 1..=1, ()).ok();

    // Wrap mpv's parent surface as a Proxy. Foreign object — never
    // destroyed on our side (Drop on a non-rust-managed ObjectId is a
    // no-op in wayland-backend).
    let parent_id = unsafe {
        ObjectId::from_ptr(WlSurface::interface(), parent_surface_ptr.cast())
            .map_err(|_| "parent surface interface mismatch")?
    };
    let parent = WlSurface::from_id(&conn, parent_id)
        .map_err(|_| "parent surface from_id failed".to_string())?;

    let state = WlState {
        conn,
        qh,
        queue,
        compositor,
        subcompositor,
        shm,
        dmabuf,
        viewporter,
        parent,
        stack: Vec::new(),
        was_fullscreen: false,
        transitioning: false,
        present_mode: PresentMode::Attach,
        // SAFETY: caller guaranteed `display_ptr` is a live
        // `*mut wl_display`.
        display_ptr: NonNull::new(display_ptr).expect("display_ptr is non-null"),
        gpu_ctx: None,
        use_gpu_paint: false,
    };

    STATE
        .set(Mutex::new(state))
        .map_err(|_| "wl_state lost init race".to_string())?;
    Ok(())
}

// =====================================================================
// Helpers
// =====================================================================

impl WlState {
    pub(crate) fn flush(&self) {
        let _ = self.conn.flush();
    }
}

/// Install the shared [`GpuContext`] and flip `use_gpu_paint`. Called
/// from `lifecycle` once the EGL probe has decided (or been bypassed
/// by `--wayland-paint`).
pub fn install_gpu_paint(ctx: Arc<GpuContext>) {
    let mut st = lock();
    st.gpu_ctx = Some(ctx);
    st.use_gpu_paint = true;
}

pub(crate) fn size_in_tolerance(s: &PlatformSurface, vw: i32, vh: i32) -> bool {
    if s.pw <= 0 {
        return true;
    }
    (vw - s.pw).abs() <= TRANSITION_TOLERANCE_TEXELS
        && (vh - s.ph).abs() <= TRANSITION_TOLERANCE_TEXELS
}

// =====================================================================
// Buffer creation
// =====================================================================

/// Create a 1×1 ARGB8888 wl_buffer filled with `(r, g, b, 0xFF)`.
pub(crate) fn create_solid_color_buffer(state: &WlState, r: u8, g: u8, b: u8) -> Option<WlBuffer> {
    let fd = memfd_anon("solid-color", 4)?;
    {
        let mut mmap = unsafe { MmapOptions::new().len(4).map_mut(&fd) }.ok()?;
        // ARGB8888 little-endian byte order = [B, G, R, A].
        mmap[0] = b;
        mmap[1] = g;
        mmap[2] = r;
        mmap[3] = 0xFF;
    }
    let pool = state.shm.create_pool(fd.as_fd(), 4, &state.qh, ());
    let buf = pool.create_buffer(0, 1, 1, 4, Format::Argb8888, &state.qh, ());
    pool.destroy();
    Some(buf)
}

/// Create a wl_shm ARGB8888 buffer from a CPU pixel array.
pub(crate) fn create_shm_buffer(
    state: &WlState,
    pixels: &[u8],
    w: i32,
    h: i32,
) -> Option<WlBuffer> {
    let stride = w.checked_mul(4)?;
    let size = stride.checked_mul(h)?;
    if size <= 0 || pixels.len() < size as usize {
        return None;
    }
    let fd = memfd_anon("cef-sw", size as usize)?;
    {
        let mut mmap = unsafe { MmapOptions::new().len(size as usize).map_mut(&fd) }.ok()?;
        mmap.copy_from_slice(&pixels[..size as usize]);
    }
    let pool = state.shm.create_pool(fd.as_fd(), size, &state.qh, ());
    let buf = pool.create_buffer(0, w, h, stride, Format::Argb8888, &state.qh, ());
    pool.destroy();
    Some(buf)
}

/// Create a dmabuf-backed wl_buffer from a single-plane fd.
pub(crate) fn create_dmabuf_buffer(
    state: &WlState,
    fd: BorrowedFd<'_>,
    stride: u32,
    modifier: u64,
    w: i32,
    h: i32,
) -> Option<WlBuffer> {
    let dmabuf = state.dmabuf.as_ref()?;
    let params: ZwpLinuxBufferParamsV1 = dmabuf.create_params(&state.qh, ());
    params.add(
        fd,
        0,
        0,
        stride,
        (modifier >> 32) as u32,
        (modifier & 0xffff_ffff) as u32,
    );
    let buf = params.create_immed(
        w,
        h,
        DRM_FORMAT_ARGB8888,
        DmabufFlags::empty(),
        &state.qh,
        (),
    );
    params.destroy();
    Some(buf)
}

/// Create a CLOEXEC anonymous memfd of the given size and truncate it.
pub(crate) fn memfd_anon(name: &str, size: usize) -> Option<OwnedFd> {
    let c = std::ffi::CString::new(name).ok()?;
    let raw = unsafe { libc::memfd_create(c.as_ptr(), libc::MFD_CLOEXEC) };
    if raw < 0 {
        return None;
    }
    let owned = unsafe { OwnedFd::from_raw_fd(raw) };
    if unsafe { libc::ftruncate(owned.as_raw_fd(), size as libc::off_t) } < 0 {
        return None;
    }
    Some(owned)
}
