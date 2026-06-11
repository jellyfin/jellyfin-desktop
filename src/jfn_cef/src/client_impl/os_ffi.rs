//! Per-OS CEF handler-callback signature types. Each must match the native
//! handle type CEF passes across its C ABI for that target, or the
//! `wrap_*_handler!` expansion declares an `extern "C"` fn with the wrong
//! signature.

// macOS uses raw `*mut u8` handles and never touches `sys`.
#[cfg(not(target_os = "macos"))]
use cef::*;

#[cfg(target_os = "linux")]
pub type CursorHandle = std::os::raw::c_ulong;
#[cfg(target_os = "macos")]
pub type CursorHandle = *mut u8;
#[cfg(target_os = "windows")]
pub type CursorHandle = sys::HCURSOR;

#[cfg(target_os = "linux")]
pub type OsKeyEvent<'a> = Option<&'a mut sys::XEvent>;
#[cfg(target_os = "macos")]
pub type OsKeyEvent<'a> = *mut u8;
#[cfg(target_os = "windows")]
pub type OsKeyEvent<'a> = Option<&'a mut sys::MSG>;
