use std::ffi::c_void;
use std::os::fd::RawFd;
use std::ptr::NonNull;

/// Where the painter should attach its swapchain. Caller passes raw
/// platform handles; the painter wraps them in raw-window-handle types.
pub enum WindowTarget {
    /// X11 (xcb) — `connection` is an `xcb_connection_t*`, `window` is the
    /// XID. `visual` is the ARGB visual ID. `screen` is the screen index.
    Xcb {
        connection: NonNull<c_void>,
        window: u32,
        screen: i32,
        visual: u32,
    },
    /// Wayland — `display` is `wl_display*`, `surface` is `wl_surface*`.
    /// Once a wl_surface is handed to Vulkan WSI, no other client code
    /// may call `wl_surface_attach`/`commit` on it; presents go through
    /// the swapchain only.
    Wayland {
        display: NonNull<c_void>,
        surface: NonNull<c_void>,
    },
}

// Both variants only carry pointers the caller already keeps alive for
// the lifetime of the painter; the painter never derefs without the
// caller's coordination.
unsafe impl Send for WindowTarget {}

#[derive(Copy, Clone, Debug)]
pub struct DirtyRect {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

#[derive(Copy, Clone)]
pub struct DmabufPlane {
    pub fd: RawFd,
    pub offset: u32,
    pub stride: u32,
}

/// Reserved for the v1 dmabuf-import path. v0 only routes
/// [`PixelFrame`]; call sites already pass `DmabufFrame` so the wiring
/// does not change shape when v1 lands.
pub struct DmabufFrame<'a> {
    pub width: u32,
    pub height: u32,
    pub fourcc: u32,
    pub modifier: u64,
    pub planes: &'a [DmabufPlane],
    pub buffer_id: u64,
    pub dirty: &'a [DirtyRect],
}

pub struct PixelFrame<'a> {
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub bgra: &'a [u8],
    pub dirty: &'a [DirtyRect],
}
