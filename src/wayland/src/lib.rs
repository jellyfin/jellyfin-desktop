//! Wayland subsystem: clipboard, input, KDE decoration palette, output-scale probe.

#![cfg(target_os = "linux")]

pub mod clipboard;
pub(crate) mod context_menu;
pub(crate) mod dropdown;
pub(crate) mod gpu_paint_worker;
pub mod input;
pub mod input_lifecycle;
#[cfg(feature = "kde-palette")]
pub mod kde_palette;
pub mod lifecycle;
pub mod make_platform;
pub mod paint_override;
pub(crate) mod popup;
pub mod proxy;
pub mod scale_probe;
pub(crate) mod scene;
pub(crate) mod shm_paint_worker;
pub mod wl_ffi;
pub mod wl_ops;
pub mod wl_state;

pub use paint_override::{WlPaintOverride, paint_override, set_paint_override};
