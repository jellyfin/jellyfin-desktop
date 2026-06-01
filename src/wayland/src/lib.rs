//! Wayland subsystem: clipboard, input, KDE decoration palette, output-scale probe.

#![cfg(target_os = "linux")]

pub mod clipboard;
pub mod dmabuf_probe;
pub mod egl_dyn;
pub(crate) mod gpu_paint_worker;
pub mod input;
pub mod input_lifecycle;
#[cfg(feature = "kde-palette")]
pub mod kde_palette;
pub mod lifecycle;
pub mod make_platform;
pub mod paint_override;
pub mod proxy;
pub mod scale_probe;
pub(crate) mod shm_paint_worker;
pub mod wl_ffi;
pub mod wl_ops;
pub mod wl_state;

pub use paint_override::{WlPaintOverride, paint_override, set_paint_override};
