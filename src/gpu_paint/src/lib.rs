//! Shared Vulkan-via-wgpu compositor for CEF OSD paint.
//!
//! Two consumers: the X11 backend (no GPU paint path today) and the
//! Wayland backend's EGL-probe-failed branch (previously fell to
//! `wl_shm` CPU upload). This crate owns a single shared [`GpuContext`]
//! and one [`GpuPainter`] per CEF surface.

#![cfg(target_os = "linux")]

mod context;
mod dmabuf_import;
mod error;
mod painter;
mod types;

pub use context::{Capabilities, GpuContext};
pub use error::GpuPaintError;
pub use painter::GpuPainter;
pub use types::{DirtyRect, DmabufFormat, DmabufFrame, DmabufPlane, PixelFrame, WindowTarget};
