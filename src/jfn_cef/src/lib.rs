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
pub mod app_menu;
mod bridge;
pub mod browsers;
pub mod business_about;
pub mod business_overlay;
pub mod business_web;
pub mod client;
mod client_impl;
mod embedded_js;
pub mod ffi;
pub mod injection;
pub mod platform_ops;
#[cfg(target_os = "macos")]
mod pump;
mod resource;
mod state;
mod v8_handler;

pub use client::{
    BeforeCloseFn, ContextBuilderFn, ContextDispatcherFn, CreatedFn, JfnCefLayer, MessageFn,
};
pub use ffi::*;

pub const APP_VERSION: &str = env!("JFN_APP_VERSION");
pub const APP_VERSION_FULL: &str = env!("JFN_APP_VERSION_FULL");
pub const APP_CEF_VERSION: &str = env!("JFN_APP_CEF_VERSION");
