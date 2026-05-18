//! Per-user filesystem locations. Mirrors the layout previously produced by
//! `src/paths/{linux,macos,windows}.cpp`:
//!
//! - Linux: XDG Base Directory (config/cache/state) with `$HOME` fallback.
//! - macOS: `~/.config` for config (matches existing installs), `~/Library`
//!   for cache/logs.
//! - Windows: `%APPDATA%` for config, `%LOCALAPPDATA%` for cache/logs.
//!
//! Each directory getter creates the directory (and parents) if missing
//! before returning. Strings are heap-allocated C strings owned by Rust and
//! freed via `jfn_paths_free`.

use std::env;
use std::ffi::{CString, c_char};
use std::fs;
use std::path::PathBuf;
use std::process::Command;
use std::ptr;

const APP_DIR_NAME: &str = "jellyfin-desktop";
const LOG_FILE_NAME: &str = "jellyfin-desktop.log";

fn env_or(var: &str, fallback: &str) -> String {
    match env::var(var) {
        Ok(v) if !v.is_empty() => v,
        _ => fallback.to_string(),
    }
}

fn home() -> String {
    env_or("HOME", "/tmp")
}

fn ensure(path: PathBuf) -> PathBuf {
    let _ = fs::create_dir_all(&path);
    path
}

#[cfg(target_os = "linux")]
fn xdg_or_home(xdg_var: &str, home_subdir: &str) -> PathBuf {
    let fallback = format!("{}{}", home(), home_subdir);
    PathBuf::from(env_or(xdg_var, &fallback))
}

#[cfg(target_os = "linux")]
fn config_dir() -> PathBuf {
    ensure(xdg_or_home("XDG_CONFIG_HOME", "/.config").join(APP_DIR_NAME))
}

#[cfg(target_os = "linux")]
fn cache_dir() -> PathBuf {
    ensure(xdg_or_home("XDG_CACHE_HOME", "/.cache").join(APP_DIR_NAME))
}

#[cfg(target_os = "linux")]
fn log_dir() -> PathBuf {
    ensure(xdg_or_home("XDG_STATE_HOME", "/.local/state").join(APP_DIR_NAME))
}

#[cfg(target_os = "macos")]
fn config_dir() -> PathBuf {
    let base = env_or("XDG_CONFIG_HOME", &format!("{}/.config", home()));
    ensure(PathBuf::from(base).join(APP_DIR_NAME))
}

#[cfg(target_os = "macos")]
fn cache_dir() -> PathBuf {
    ensure(
        PathBuf::from(home())
            .join("Library/Caches")
            .join(APP_DIR_NAME),
    )
}

#[cfg(target_os = "macos")]
fn log_dir() -> PathBuf {
    ensure(
        PathBuf::from(home())
            .join("Library/Logs")
            .join(APP_DIR_NAME),
    )
}

#[cfg(windows)]
fn config_dir() -> PathBuf {
    ensure(PathBuf::from(env_or("APPDATA", "C:")).join(APP_DIR_NAME))
}

#[cfg(windows)]
fn local_appdata() -> String {
    env_or("LOCALAPPDATA", &env_or("APPDATA", "C:"))
}

#[cfg(windows)]
fn cache_dir() -> PathBuf {
    ensure(PathBuf::from(local_appdata()).join(APP_DIR_NAME))
}

#[cfg(windows)]
fn log_dir() -> PathBuf {
    ensure(
        PathBuf::from(local_appdata())
            .join(APP_DIR_NAME)
            .join("Logs"),
    )
}

fn mpv_home() -> PathBuf {
    ensure(config_dir().join("mpv"))
}

fn log_path() -> PathBuf {
    log_dir().join(LOG_FILE_NAME)
}

fn to_c(path: PathBuf) -> *mut c_char {
    let s = path.to_string_lossy().into_owned();
    CString::new(s)
        .map(|c| c.into_raw())
        .unwrap_or(ptr::null_mut())
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_paths_config_dir() -> *mut c_char {
    to_c(config_dir())
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_paths_cache_dir() -> *mut c_char {
    to_c(cache_dir())
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_paths_log_dir() -> *mut c_char {
    to_c(log_dir())
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_paths_log_path() -> *mut c_char {
    to_c(log_path())
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_paths_mpv_home() -> *mut c_char {
    to_c(mpv_home())
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_paths_open_mpv_home() {
    let path = mpv_home();
    #[cfg(target_os = "linux")]
    {
        let _ = Command::new("xdg-open").arg(&path).spawn();
    }
    #[cfg(target_os = "macos")]
    {
        let _ = Command::new("open").arg(&path).spawn();
    }
    #[cfg(windows)]
    {
        // Match the legacy C++ behavior: native backslash-separated path
        // handed to Explorer (ShellExecuteA "explore"). `explorer.exe`
        // accepts the same and opens the folder in a new window.
        let native: String = path
            .to_string_lossy()
            .chars()
            .map(|c| if c == '/' { '\\' } else { c })
            .collect();
        let _ = Command::new("explorer").arg(native).spawn();
    }
}

/// Free a string previously returned by one of the path getters.
///
/// # Safety
/// `s` must have been obtained from a `jfn_paths_*` function and not yet freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_paths_free(s: *mut c_char) {
    if !s.is_null() {
        unsafe { drop(CString::from_raw(s)) };
    }
}
