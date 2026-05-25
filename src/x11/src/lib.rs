//! X11 platform subsystem: surface management, input thread, Platform impl.

#![cfg(target_os = "linux")]

pub mod input;
pub mod input_lifecycle;
pub mod lifecycle;
pub mod make_platform;
pub mod shm;
pub mod surface;
pub mod x11_state;
