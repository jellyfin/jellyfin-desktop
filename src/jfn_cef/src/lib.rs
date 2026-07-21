//! CEF process bootstrap + App handlers.

mod app;
pub mod app_menu;
pub mod bridge;
pub mod browsers;
pub mod business_about;
mod business_common;
pub mod business_overlay;
pub mod business_web;
pub mod client;
mod client_impl;
mod embedded_js;
pub mod ffi;
pub mod injection;
mod ipc;
mod menu_ownership;
mod paint_scheduler;
pub mod platform_ops;
mod resource;
pub mod sink_routing;
mod state;
mod v8_handler;
pub mod version;
pub mod window_controls;
mod window_sync;

pub use client::{BeforeCloseFn, ContextBuilderFn, ContextDispatcherFn, CreatedFn, JfnCefLayer};
pub use ffi::*;

pub const APP_VERSION: &str = env!("JFN_APP_VERSION");
pub const APP_VERSION_FULL: &str = env!("JFN_APP_VERSION_FULL");
pub use version::cef_version;
