//! Wayland subsystem: clipboard, input, KDE decoration palette, output-scale probe.

#![cfg(target_os = "linux")]

pub(crate) mod app_conn;
pub(crate) mod clipboard;
pub(crate) mod context_menu;
pub(crate) mod decoration_probe;
pub(crate) mod dropdown;
pub(crate) mod gpu_paint_worker;
pub(crate) mod input;
pub(crate) mod input_lifecycle;
#[cfg(feature = "kde-palette")]
pub(crate) mod kde_palette;
pub(crate) mod lifecycle;
pub mod make_platform;
pub(crate) mod mpv_host;
pub(crate) mod mpv_proxy;
pub mod paint_override;
pub(crate) mod popup;
pub(crate) mod root_window;
pub(crate) mod scale;
pub(crate) mod scale_probe;
pub(crate) mod scene;
pub(crate) mod shm_paint_worker;
pub(crate) mod window_source;
pub(crate) mod window_state;
pub(crate) mod wl_ops;
pub(crate) mod wl_state;

pub use paint_override::{WlPaintOverride, paint_override, set_paint_override};
