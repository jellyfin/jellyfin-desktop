//! Shared X11 state: connection, atoms, ARGB visual, per-surface table.
//!
//! Connection lives behind an `Arc` so the input thread can hold a
//! reference independent of the global mutex. The mutex protects every
//! mutable field including the `live` surface list.

use parking_lot::Mutex;
use std::sync::{Arc, OnceLock};

use jfn_gpu_paint::{Capabilities, GpuContext, GpuPainter};
use xcb::{Xid, XidNew, x};

/// Owns one SHM segment + the mapped memory. Two per surface so the
/// renderer can double-buffer.
pub struct ShmBuffer {
    pub seg: xcb::shm::Seg,
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
            seg: xcb::shm::Seg::new(0),
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

/// Per-CefLayer surface. Each is a top-level ARGB override-redirect
/// window positioned over mpv's window.
pub struct PlatformSurface {
    pub window: x::Window,
    pub gc: x::Gcontext,
    pub bufs: [ShmBuffer; 2],
    pub buf_idx: usize,
    pub visible: bool,
    pub pw: i32,
    pub ph: i32,
    /// GPU compositor, lazily created on the first software present
    /// when a [`GpuContext`] is available. Falls back to SHM if init
    /// fails.
    pub painter: Option<GpuPainter>,
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
            window: x::Window::new(0),
            gc: x::Gcontext::new(0),
            bufs: [ShmBuffer::default(), ShmBuffer::default()],
            buf_idx: 0,
            visible: true,
            pw: 0,
            ph: 0,
            painter: None,
        }
    }
}

#[derive(Copy, Clone)]
pub struct Atoms {
    pub net_wm_opacity: x::Atom,
    pub net_wm_window_type: x::Atom,
    pub net_wm_window_type_notification: x::Atom,
    pub net_wm_state: x::Atom,
    pub net_wm_state_above: x::Atom,
    pub net_wm_state_skip_taskbar: x::Atom,
    pub net_wm_state_skip_pager: x::Atom,
    pub wm_protocols: x::Atom,
    pub wm_delete_window: x::Atom,
}

pub struct Mutable {
    pub screen_num: i32,
    pub root: x::Window,
    pub argb_visual: x::Visualid,
    pub argb_depth: u8,
    pub colormap: x::Colormap,
    pub parent: x::Window,
    pub parent_x: i32,
    pub parent_y: i32,
    pub pw: i32,
    pub ph: i32,
    pub cached_scale: f32,
    pub atoms: Atoms,
    pub live: Vec<*mut PlatformSurface>,
    /// Shared GPU compositor. `None` when no Vulkan adapter was found
    /// at init; in that case surface presents fall back to SHM.
    pub gpu_ctx: Option<Arc<GpuContext>>,
    pub gpu_caps: Capabilities,
}

unsafe impl Send for Mutable {}

pub static CONN: OnceLock<Arc<xcb::Connection>> = OnceLock::new();
pub static MUT: Mutex<Option<Mutable>> = Mutex::new(None);

pub fn conn() -> Option<Arc<xcb::Connection>> {
    CONN.get().cloned()
}

pub fn is_none_window(w: x::Window) -> bool {
    w.resource_id() == 0
}

pub fn is_none_gc(g: x::Gcontext) -> bool {
    g.resource_id() == 0
}
