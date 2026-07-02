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
use wayland_backend::client::Backend;
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

/// A layer's synchronized subsurface.
pub(crate) struct SyncSubsurface(WlSubsurface);

impl SyncSubsurface {
    pub(crate) fn create(
        subcompositor: &WlSubcompositor,
        surface: &WlSurface,
        parent: &WlSurface,
        qh: &QueueHandle<DispatchState>,
    ) -> Self {
        let sub = subcompositor.get_subsurface(surface, parent, qh, ());
        Self(sub)
    }

    pub(crate) fn set_position(&self, x: i32, y: i32) {
        self.0.set_position(x, y);
    }

    pub(crate) fn place_above(&self, sibling: &WlSurface) {
        self.0.place_above(sibling);
    }

    pub(crate) fn destroy(self) {
        self.0.destroy();
    }
}

pub(crate) struct DmabufBuf {
    pub id: (u64, u64),
    pub w: i32,
    pub h: i32,
    pub stride: u32,
    pub modifier: u64,
    pub buf: WlBuffer,
}

pub(crate) enum DmabufLease {
    Pooled(WlBuffer),
    OneShot(WlBuffer),
}

pub(crate) struct PlatformSurface {
    pub surface: Option<WlSurface>,
    pub subsurface: Option<SyncSubsurface>,
    pub viewport: Option<WpViewport>,
    pub buffer: Option<WlBuffer>,
    pub dmabuf_pool: Vec<DmabufBuf>,
    pub buffer_w: i32,
    pub buffer_h: i32,
    pub visible: bool,
    pub placeholder: bool,
    pub null_attached: bool,

    // Popup child.
    pub popup_surface: Option<WlSurface>,
    pub popup_subsurface: Option<SyncSubsurface>,
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
            dmabuf_pool: Vec::new(),
            buffer_w: 0,
            buffer_h: 0,
            visible: true,
            placeholder: false,
            null_attached: false,
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

    pub root_surface: Option<WlSurface>,

    /// Stack order, bottom-to-top. Raw pointers are valid for the
    /// lifetime of each `PlatformSurface` (heap-allocated via `Box`,
    /// removed before drop).
    pub stack: Vec<*mut PlatformSurface>,

    pub was_fullscreen: bool,

    /// Raw `wl_display*` — kept so `GpuPainter::new` can build
    /// `VK_KHR_wayland_surface` handles for child surfaces.
    pub display_ptr: NonNull<c_void>,
    pub gpu_ctx: Option<Arc<GpuContext>>,
    /// When true, `surface_present_software` routes through each
    /// surface's GPU paint worker (Vulkan WSI) instead of `wl_shm`.
    /// `set_visible` and `resize` also skip their
    /// `wl_surface.attach`/`viewport.set_destination` work for the
    /// gpu_paint surface.
    pub use_gpu_paint: bool,

    pub scene: crate::scene::Scene,
    pub menu_io: crate::popup::MenuIo,
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

// Post-init accessor; `try_state()` is the fallible sibling for early paths.
#[allow(clippy::expect_used)] // boot invariant: init runs before any lock()
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
    ZwpLinuxDmabufV1,
    ZwpLinuxBufferParamsV1,
    WpViewporter,
    WpViewport,
);

// Under a synchronized subsurface the compositor keeps reading a buffer for a
// frame after it is replaced. Destroying or re-attaching one while `released`
// is false shows a blank frame.
struct ManagedBuffer {
    buf: WlBuffer,
    released: bool,
    doomed: bool,
}

static MANAGED: Mutex<Vec<ManagedBuffer>> = Mutex::new(Vec::new());

pub(crate) fn track_buffer(buf: &WlBuffer) {
    MANAGED.lock().push(ManagedBuffer {
        buf: buf.clone(),
        released: true,
        doomed: false,
    });
}

pub(crate) fn mark_attached(buf: &WlBuffer) {
    let mut mgd = MANAGED.lock();
    if let Some(m) = mgd.iter_mut().find(|m| &m.buf == buf) {
        m.released = false;
    }
}

pub(crate) fn buffer_is_idle(buf: &WlBuffer) -> bool {
    MANAGED
        .lock()
        .iter()
        .find(|m| &m.buf == buf)
        .is_some_and(|m| m.released)
}

pub(crate) fn retire_buffer(buf: WlBuffer) {
    let mut mgd = MANAGED.lock();
    match mgd.iter().position(|m| m.buf == buf) {
        Some(pos) if mgd[pos].released => {
            mgd.swap_remove(pos).buf.destroy();
        }
        Some(pos) => mgd[pos].doomed = true,
        None => {
            debug_assert!(
                false,
                "retire_buffer: untracked buffer — a release may have been missed"
            );
            tracing::error!("retire_buffer: untracked buffer — a release may have been missed");
            mgd.push(ManagedBuffer {
                buf,
                released: false,
                doomed: true,
            });
        }
    }
}

pub(crate) fn note_buffer_release(buffer: &WlBuffer) {
    let mut mgd = MANAGED.lock();
    if let Some(pos) = mgd.iter().position(|m| m.buf == *buffer) {
        if mgd[pos].doomed {
            mgd.swap_remove(pos).buf.destroy();
        } else {
            mgd[pos].released = true;
        }
    }
}

impl Dispatch<WlBuffer, ()> for DispatchState {
    fn event(
        _: &mut Self,
        buffer: &WlBuffer,
        event: <WlBuffer as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wayland_client::protocol::wl_buffer::Event::Release = event {
            note_buffer_release(buffer);
        }
    }
}

/// Dispatch the CEF connection's pending events (notably `wl_buffer.release`).
/// Called from the root-window read loop, the only reader of the shared display.
pub(crate) fn pump_events() {
    if let Some(state) = STATE.get() {
        let mut st = state.lock();
        let st = &mut *st;
        let _ = st.queue.dispatch_pending(&mut DispatchState);
    }
}

// =====================================================================
// Init — bind globals against a dedicated EventQueue over the foreign
// (mpv-owned) wl_display.
// =====================================================================

/// SAFETY: `display_ptr` must be a live `*mut wl_display` owned by mpv.
pub(crate) unsafe fn init(display_ptr: *mut c_void) -> Result<(), String> {
    if STATE.get().is_some() {
        return Err("wl_state already initialised".into());
    }
    if display_ptr.is_null() {
        return Err("null display".into());
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

    let mut state = WlState {
        conn,
        qh,
        queue,
        compositor,
        subcompositor,
        shm,
        dmabuf,
        viewporter,
        root_surface: None,
        stack: Vec::new(),
        was_fullscreen: false,
        // SAFETY: caller guaranteed `display_ptr` is a live
        // `*mut wl_display`.
        display_ptr: NonNull::new(display_ptr).ok_or_else(|| "display_ptr is null".to_string())?,
        gpu_ctx: None,
        use_gpu_paint: false,
        scene: crate::scene::Scene::default(),
        menu_io: crate::popup::MenuIo::default(),
    };

    ensure_root_locked(&mut state);

    STATE
        .set(Mutex::new(state))
        .map_err(|_| "wl_state lost init race".to_string())?;
    Ok(())
}

fn surface_from_ptr(
    conn: &Connection,
    raw: *mut std::ffi::c_void,
    what: &str,
) -> Option<WlSurface> {
    if raw.is_null() {
        return None;
    }
    // SAFETY: `raw` must be a live `wl_proxy*` for a `wl_surface` on the same
    // `wl_display` backing `conn` (published by root_window / mpv_proxy).
    let id = match unsafe {
        wayland_client::backend::ObjectId::from_ptr(WlSurface::interface(), raw.cast())
    } {
        Ok(id) => id,
        Err(e) => {
            tracing::error!(target: "Main", "{what}: ObjectId::from_ptr: {e}");
            return None;
        }
    };
    match WlSurface::from_id(conn, id) {
        Ok(s) => Some(s),
        Err(e) => {
            tracing::error!(target: "Main", "{what}: WlSurface::from_id: {e}");
            None
        }
    }
}

fn parent_layer_locked(st: &mut WlState, ptr: *mut PlatformSurface) {
    if ptr.is_null() {
        return;
    }
    let Some(root) = st.root_surface.clone() else {
        return;
    };
    // SAFETY: live PlatformSurface address held in `stack`, accessed under the lock.
    let s = unsafe { &mut *ptr };
    if s.subsurface.is_some() {
        return;
    }
    let Some(surface) = s.surface.as_ref() else {
        return;
    };
    let sub = SyncSubsurface::create(&st.subcompositor, surface, &root, &st.qh);
    sub.set_position(0, 0);
    s.subsurface = Some(sub);
}

pub(crate) fn ensure_root_locked(st: &mut WlState) {
    if st.root_surface.is_some() {
        return;
    }
    let raw = crate::root_window::root_surface_ptr();
    let Some(root) = surface_from_ptr(&st.conn, raw, "overlay root") else {
        return;
    };
    st.root_surface = Some(root);

    let pending: Vec<*mut PlatformSurface> = st.stack.clone();
    for ptr in pending {
        parent_layer_locked(st, ptr);
    }
    tracing::info!(target: "Main", "CEF layers parented under app root");
}

pub(crate) fn parent_layer(st: &mut WlState, ptr: *mut PlatformSurface) {
    parent_layer_locked(st, ptr);
}

// =====================================================================
// Helpers
// =====================================================================

impl WlState {
    pub(crate) fn flush(&self) {
        let _ = self.conn.flush();
    }
}

pub fn install_gpu_paint(ctx: Arc<GpuContext>) {
    let mut st = lock();
    st.gpu_ctx = Some(ctx);
    st.use_gpu_paint = true;
}

// Does an incoming frame's visible size match the authoritative physical window
// size (within tolerance)? Reads the single source, not a per-layer copy.
pub(crate) fn size_in_tolerance(vw: i32, vh: i32) -> bool {
    let (pw, ph) = crate::window_state::jfn_wl_window_size();
    if pw <= 0 {
        return true;
    }
    (vw - pw).abs() <= TRANSITION_TOLERANCE_TEXELS && (vh - ph).abs() <= TRANSITION_TOLERANCE_TEXELS
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
    track_buffer(&buf);
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
    track_buffer(&buf);
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
    track_buffer(&buf);
    Some(buf)
}

const DMABUF_POOL_CAP: usize = 16;

fn create_dmabuf_for_frame(
    state: &WlState,
    frame: &crate::wl_ops::JfnDmabufFrame,
) -> Option<WlBuffer> {
    create_dmabuf_buffer(
        state,
        frame.fd.as_fd(),
        frame.stride,
        frame.modifier,
        frame.coded_w,
        frame.coded_h,
    )
}

pub(crate) fn get_or_create_dmabuf(
    state: &WlState,
    s: &mut PlatformSurface,
    frame: &crate::wl_ops::JfnDmabufFrame,
) -> Option<DmabufLease> {
    let Some(id) = frame.id else {
        return Some(DmabufLease::OneShot(create_dmabuf_for_frame(state, frame)?));
    };

    let hit = s.dmabuf_pool.iter().position(|e| {
        e.id == id
            && e.w == frame.coded_w
            && e.h == frame.coded_h
            && e.stride == frame.stride
            && e.modifier == frame.modifier
    });
    if let Some(pos) = hit {
        if buffer_is_idle(&s.dmabuf_pool[pos].buf) {
            let entry = s.dmabuf_pool.remove(pos);
            s.dmabuf_pool.insert(0, entry);
            return Some(DmabufLease::Pooled(s.dmabuf_pool[0].buf.clone()));
        }
        retire_buffer(s.dmabuf_pool.remove(pos).buf);
    }
    if let Some(stale) = s.dmabuf_pool.iter().position(|e| e.id == id) {
        retire_buffer(s.dmabuf_pool.remove(stale).buf);
    }

    let buf = create_dmabuf_for_frame(state, frame)?;
    let lease = buf.clone();
    s.dmabuf_pool.insert(
        0,
        DmabufBuf {
            id,
            w: frame.coded_w,
            h: frame.coded_h,
            stride: frame.stride,
            modifier: frame.modifier,
            buf,
        },
    );
    while s.dmabuf_pool.len() > DMABUF_POOL_CAP {
        if let Some(evicted) = s.dmabuf_pool.pop() {
            retire_buffer(evicted.buf);
        }
    }
    Some(DmabufLease::Pooled(lease))
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
