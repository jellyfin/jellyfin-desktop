//! CEF process bootstrap + App handlers.
//!
//! Ports `src/cef/cef_app.cpp` to Rust. C ABI in [`ffi`] mirrors the
//! `CefRuntime::` namespace declared in `src/cef/cef_app.h` so the C++ side
//! is a thin shim during the transition.
//!
//! NOTE: this is the initial slice — bootstrap + App skeleton + scheme
//! registration + context-initialized callback hand-off. Render-process V8
//! injection, popup DOM walk, and macOS message pump are deferred to
//! follow-up commits.

mod app;
mod bridge;
mod client;
mod embedded_js;
pub mod ffi;
#[cfg(target_os = "macos")]
mod pump;
mod resource;
mod state;
mod v8_handler;

pub use ffi::*;

pub const APP_VERSION: &str = env!("JFN_APP_VERSION");
pub const APP_VERSION_FULL: &str = env!("JFN_APP_VERSION_FULL");
pub const APP_CEF_VERSION: &str = env!("JFN_APP_CEF_VERSION");
