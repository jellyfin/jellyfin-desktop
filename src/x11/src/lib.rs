//! X11 platform subsystem: surface management, input thread, Platform impl.

#![cfg(target_os = "linux")]

pub mod geometry;
pub(crate) mod gpu_paint_worker;
pub(crate) mod input;
pub(crate) mod input_lifecycle;
pub mod lifecycle;
pub mod make_platform;
pub mod overlay_fsm;
pub mod paint_override;
pub(crate) mod scale;
pub mod shm;
pub(crate) mod shm_paint_worker;
pub mod surface;
pub(crate) mod x11_state;

pub use paint_override::{X11PaintOverride, paint_override, set_paint_override};
