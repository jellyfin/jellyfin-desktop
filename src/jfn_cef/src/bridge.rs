#![allow(dead_code)]
//! Thin Rust wrappers around the C FFI symbols exposed by sibling Rust
//! crates (jfn-config, jfn-paths, jfn-logging). The dep crates only export
//! `extern "C"` surfaces today; rather than refactor them to expose `pub`
//! Rust APIs in this slice, we re-link to the same symbols.
//!
//! TODO: promote each dep crate's internal Rust functions to `pub` and
//! drop this module.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;

// Symbols are pulled in through Cargo workspace deps (jfn-paths, jfn-config,
// jfn-logging). No #[link] attribute — rustc resolves them through the rlib
// dep chain when the umbrella `jfn-rust` staticlib is linked.
unsafe extern "C" {
    fn jfn_paths_config_dir() -> *mut c_char;
    fn jfn_paths_cache_dir() -> *mut c_char;
    fn jfn_paths_free(s: *mut c_char);

    fn jfn_settings_init(path: *const c_char);
    fn jfn_settings_load() -> bool;
    fn jfn_settings_get_server_url() -> *mut c_char;
    fn jfn_settings_cli_json(
        platform_default: *const c_char,
        hwdec_opts: *const *const c_char,
        hwdec_count: usize,
    ) -> *mut c_char;
    fn jfn_settings_free_string(s: *mut c_char);

    fn jfn_log(category: u8, level: u8, msg: *const c_char, len: usize);
    fn jfn_log_enabled(category: u8, level: u8) -> bool;
    fn jfn_log_active_path() -> *mut c_char;
    fn jfn_log_free_string(s: *mut c_char);
}

// Category constants — must match the LogCategory enum in src/logging.h.
pub const LOG_CEF: u8 = 2;
pub const LOG_RESOURCE: u8 = 6;

// Level constants — must match LogLevel in src/logging.h.
pub const LEVEL_DEBUG: u8 = 1;
pub const LEVEL_INFO: u8 = 2;
pub const LEVEL_WARN: u8 = 3;

fn take_c_string(s: *mut c_char, free: unsafe extern "C" fn(*mut c_char)) -> String {
    if s.is_null() {
        return String::new();
    }
    let out = unsafe { CStr::from_ptr(s) }.to_string_lossy().into_owned();
    unsafe { free(s) };
    out
}

pub fn paths_config_dir() -> String {
    take_c_string(unsafe { jfn_paths_config_dir() }, jfn_paths_free)
}

pub fn paths_cache_dir() -> String {
    take_c_string(unsafe { jfn_paths_cache_dir() }, jfn_paths_free)
}

pub fn settings_init(path: &str) {
    let c = CString::new(path).unwrap();
    unsafe { jfn_settings_init(c.as_ptr()) };
}

pub fn settings_load() -> bool {
    unsafe { jfn_settings_load() }
}

pub fn settings_server_url() -> String {
    take_c_string(unsafe { jfn_settings_get_server_url() }, jfn_settings_free_string)
}

pub fn settings_cli_json(device_name: &str, hwdec_opts: &[&str]) -> String {
    let device_c = CString::new(device_name).unwrap();
    let hwdec_cs: Vec<CString> = hwdec_opts
        .iter()
        .map(|s| CString::new(*s).unwrap())
        .collect();
    let ptrs: Vec<*const c_char> = hwdec_cs.iter().map(|s| s.as_ptr()).collect();
    let p = unsafe {
        jfn_settings_cli_json(
            device_c.as_ptr(),
            if ptrs.is_empty() {
                std::ptr::null()
            } else {
                ptrs.as_ptr()
            },
            ptrs.len(),
        )
    };
    take_c_string(p, jfn_settings_free_string)
}

pub fn log(category: u8, level: u8, msg: &str) {
    let c = CString::new(msg).unwrap_or_else(|_| CString::new("<log msg with NUL>").unwrap());
    let bytes = c.as_bytes();
    unsafe { jfn_log(category, level, bytes.as_ptr() as *const c_char, bytes.len()) };
}

pub fn log_enabled(category: u8, level: u8) -> bool {
    unsafe { jfn_log_enabled(category, level) }
}

pub fn log_active_path() -> String {
    take_c_string(unsafe { jfn_log_active_path() }, jfn_log_free_string)
}
