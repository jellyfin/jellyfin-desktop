//! Shared X11 state: connection, atoms, ARGB visual, per-surface table.
//!
//! Connection lives behind an `Arc` so the input thread can hold a
//! reference independent of the global mutex. The mutex protects every
//! mutable field including the `live` surface list.

use parking_lot::Mutex;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::{Arc, OnceLock};

use jfn_gpu_paint::{Capabilities, GpuContext};

use crate::gpu_paint_worker::X11GpuPaintWorker;
use crate::shm_paint_worker::X11ShmPaintWorker;
use x11rb::{protocol::shm, rust_connection::RustConnection};

/// Owns one SHM segment + the mapped memory. Two per surface so the
/// renderer can double-buffer.
pub struct ShmBuffer {
    pub seg: shm::Seg,
    pub shmid: i32,
    pub data: *mut u8,
    pub w: i32,
    pub h: i32,
    pub size: usize,
}

unsafe impl Send for ShmBuffer {}

impl ShmBuffer {
    pub fn empty() -> Self {
        Self {
            seg: 0,
            shmid: -1,
            data: std::ptr::null_mut(),
            w: 0,
            h: 0,
            size: 0,
        }
    }
}

impl Default for ShmBuffer {
    fn default() -> Self {
        Self::empty()
    }
}

/// Per-CefLayer surface. Each is a top-level ARGB window positioned over
/// mpv's parent window.
pub struct PlatformSurface {
    pub window: u32,
    pub gc: u32,
    pub(crate) shm_paint_worker: Option<X11ShmPaintWorker>,
    pub visible: bool,
    pub pw: i32,
    pub ph: i32,
    /// GPU presenter worker, lazily created on the first software present
    /// when a [`GpuContext`] is available. Falls back to SHM if init or
    /// present fails.
    pub(crate) gpu_paint_worker: Option<X11GpuPaintWorker>,
}

unsafe impl Send for PlatformSurface {}

impl Default for PlatformSurface {
    fn default() -> Self {
        Self::new()
    }
}

impl PlatformSurface {
    pub fn new() -> Self {
        Self {
            window: 0,
            gc: 0,
            shm_paint_worker: None,
            visible: true,
            pw: 0,
            ph: 0,
            gpu_paint_worker: None,
        }
    }
}

#[derive(Copy, Clone)]
pub struct Atoms {
    pub net_wm_window_type: u32,
    pub net_wm_window_type_normal: u32,
    pub net_wm_state: u32,
    pub net_wm_state_skip_taskbar: u32,
    pub net_wm_state_skip_pager: u32,
    pub net_wm_state_fullscreen: u32,
    pub wm_protocols: u32,
    pub wm_delete_window: u32,
    pub motif_wm_hints: u32,
    pub net_active_window: u32,
}

pub struct Mutable {
    pub screen_num: i32,
    pub root: u32,
    pub argb_visual: u32,
    pub argb_depth: u8,
    pub colormap: u32,
    pub parent: u32,
    pub parent_x: i32,
    pub parent_y: i32,
    pub pw: i32,
    pub ph: i32,
    pub parent_fullscreen: bool,
    pub cached_scale: f32,
    pub atoms: Atoms,
    pub live: Vec<*mut PlatformSurface>,
    /// Shared GPU compositor. `None` when no Vulkan adapter was found
    /// at init; in that case surface presents fall back to SHM.
    pub gpu_ctx: Option<Arc<GpuContext>>,
    pub gpu_caps: Capabilities,
    /// When set, the dmabuf-import tier is active: presents arrive via
    /// `surface_present` rather than `surface_present_software`.
    pub use_dmabuf: bool,
    /// Drops stale-size frames during a resize so the last good frame holds.
    pub gate: jfn_compositor_core::transition::TransitionGate,
}

unsafe impl Send for Mutable {}

static CONN: OnceLock<Arc<xcb::Connection>> = OnceLock::new();
pub static X11RB_CONN: OnceLock<Arc<RustConnection>> = OnceLock::new();
pub static MUT: Mutex<Option<Mutable>> = Mutex::new(None);

pub(crate) fn open_xcb_connection() -> Result<Arc<xcb::Connection>, String> {
    let conn = xcb::Connection::connect(None)
        .map(|(conn, _)| Arc::new(conn))
        .map_err(|e| format!("{e:?}"))?;
    CONN.set(conn.clone())
        .map_err(|_| "xcb connection already initialized".to_string())?;
    Ok(conn)
}

pub(crate) fn xcb_conn() -> Option<Arc<xcb::Connection>> {
    CONN.get().cloned()
}

pub fn x11rb_conn() -> Option<Arc<RustConnection>> {
    X11RB_CONN.get().cloned()
}

pub(crate) fn raw_xcb_connection() -> Option<NonNull<c_void>> {
    let conn = CONN.get()?;
    NonNull::new(conn.get_raw_conn() as *mut c_void)
}

pub fn is_none_window(w: u32) -> bool {
    w == 0
}

pub fn is_none_gc(g: u32) -> bool {
    g == 0
}
