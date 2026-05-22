//! Wayland subsystem: clipboard, input, KDE decoration palette, output-scale probe.

pub mod clipboard;
pub mod dmabuf_probe;
pub mod egl_dyn;
pub mod fade;
pub mod input;
pub mod input_lifecycle;
pub mod kde_palette;
pub mod lifecycle;
pub mod make_platform;
pub mod proxy;
pub mod scale_probe;
pub mod wl_ffi;
pub mod wl_ops;
pub mod wl_state;
