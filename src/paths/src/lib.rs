//! Per-user filesystem locations.
//!
//! - Linux: XDG Base Directory (config/cache/state) with `$HOME` fallback.
//! - macOS: `~/.config` for config (matches existing installs), `~/Library`
//!   for cache/logs.
//! - Windows: `%APPDATA%` for config, `%LOCALAPPDATA%` for cache/logs.
//!
//! Each directory getter creates the directory (and parents) if missing
//! before returning.

use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

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
pub fn config_dir() -> PathBuf {
    ensure(xdg_or_home("XDG_CONFIG_HOME", "/.config").join(APP_DIR_NAME))
}

#[cfg(target_os = "linux")]
pub fn cache_dir() -> PathBuf {
    ensure(xdg_or_home("XDG_CACHE_HOME", "/.cache").join(APP_DIR_NAME))
}

#[cfg(target_os = "linux")]
pub fn log_dir() -> PathBuf {
    ensure(xdg_or_home("XDG_STATE_HOME", "/.local/state").join(APP_DIR_NAME))
}

#[cfg(target_os = "macos")]
pub fn config_dir() -> PathBuf {
    let base = env_or("XDG_CONFIG_HOME", &format!("{}/.config", home()));
    ensure(PathBuf::from(base).join(APP_DIR_NAME))
}

#[cfg(target_os = "macos")]
pub fn cache_dir() -> PathBuf {
    ensure(
        PathBuf::from(home())
            .join("Library/Caches")
            .join(APP_DIR_NAME),
    )
}

#[cfg(target_os = "macos")]
pub fn log_dir() -> PathBuf {
    ensure(
        PathBuf::from(home())
            .join("Library/Logs")
            .join(APP_DIR_NAME),
    )
}

#[cfg(windows)]
pub fn config_dir() -> PathBuf {
    ensure(PathBuf::from(env_or("APPDATA", "C:")).join(APP_DIR_NAME))
}

#[cfg(windows)]
fn local_appdata() -> String {
    env_or("LOCALAPPDATA", &env_or("APPDATA", "C:"))
}

#[cfg(windows)]
pub fn cache_dir() -> PathBuf {
    ensure(PathBuf::from(local_appdata()).join(APP_DIR_NAME))
}

#[cfg(windows)]
pub fn log_dir() -> PathBuf {
    ensure(
        PathBuf::from(local_appdata())
            .join(APP_DIR_NAME)
            .join("Logs"),
    )
}

pub fn mpv_home() -> PathBuf {
    ensure(config_dir().join("mpv"))
}

pub fn log_path() -> PathBuf {
    log_dir().join(LOG_FILE_NAME)
}

pub fn open_mpv_home() {
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
        // explorer.exe wants native backslash-separated paths.
        let native: String = path
            .to_string_lossy()
            .chars()
            .map(|c| if c == '/' { '\\' } else { c })
            .collect();
        let _ = Command::new("explorer").arg(native).spawn();
    }
}
