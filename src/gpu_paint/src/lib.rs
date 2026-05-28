//! Shared Vulkan-via-wgpu compositor for CEF OSD paint.
//!
//! Two consumers: the X11 backend (no GPU paint path today) and the
//! Wayland backend's EGL-probe-failed branch (previously fell to
//! `wl_shm` CPU upload). This crate owns a single shared [`GpuContext`]
//! and one [`GpuPainter`] per CEF surface.
//!
//! v0 ships the pixel-upload path (CEF `OnPaint` BGRA → Vulkan staging
//! → swapchain). The dmabuf-import path (CEF `OnAcceleratedPaint` → VK
//! external memory) is staged for v1; the API already accepts
//! [`DmabufFrame`] so call sites do not have to change shape.

#![cfg(target_os = "linux")]

mod context;
mod error;
mod painter;
mod types;

pub use context::{Capabilities, GpuContext};
pub use error::GpuPaintError;
pub use painter::GpuPainter;
pub use types::{DirtyRect, DmabufFrame, DmabufPlane, PixelFrame, WindowTarget};
