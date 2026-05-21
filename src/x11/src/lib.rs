//! X11 platform subsystem: surface management, input thread, Platform vtable.
//!
//! Replaces `src/platform/x11.cpp` and `src/input/input_x11.cpp`. The
//! `make_x11_platform()` factory authors a `Platform` (mirrored repr(C)
//! from `src/platform/platform.h`) and exposes it to C++ via the same
//! cross-language vtable contract as the Wayland backend.
//!
//! Layout pin: `src/platform/platform_layout.cpp` static_asserts the
//! C++ `Platform` struct; this crate's mirror lives in
//! `crate::make_platform` and is identical to the Wayland mirror.

pub mod input;
pub mod input_lifecycle;
pub mod lifecycle;
pub mod make_platform;
pub mod shm;
pub mod surface;
pub mod x11_state;
