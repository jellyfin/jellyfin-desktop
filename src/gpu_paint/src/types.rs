use std::ffi::c_void;
use std::os::fd::OwnedFd;
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

/// Pixel layout of a CEF accelerated-paint dmabuf. CEF only ever emits
/// 8888 (its `cef_color_type_t`); we map it straight to a `VkFormat`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum DmabufFormat {
    /// BGRA8888 — matches the painter's swapchain format.
    Bgra8,
    /// RGBA8888.
    Rgba8,
}

/// One plane of a dmabuf. Owns its fd; closed on drop (Vulkan consumes a
/// dup of it at import, so the frame's copy is released afterwards).
pub struct DmabufPlane {
    pub fd: OwnedFd,
    pub offset: u64,
    pub stride: u32,
}

/// A CEF `OnAcceleratedPaint` frame, owned so it can be handed to the
/// presenter worker thread (CEF reclaims the original fd when the paint
/// callback returns, so the caller dups into `OwnedFd`). The painter
/// imports it as a Vulkan image and samples it into the swapchain — no
/// CPU copy. CEF overlay frames are single-plane.
pub struct DmabufFrame {
    pub width: u32,
    pub height: u32,
    /// CEF's visible rect within the (possibly padded) coded `width`/`height`.
    /// Used by the resize transition gate to compare against the window's
    /// expected size; the coded size can be padded larger.
    pub visible_w: u32,
    pub visible_h: u32,
    pub format: DmabufFormat,
    pub modifier: u64,
    pub planes: Vec<DmabufPlane>,
}

pub struct PixelFrame<'a> {
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub bgra: &'a [u8],
    pub dirty: &'a [DirtyRect],
}
