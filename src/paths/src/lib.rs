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
use std::io;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard, OnceLock, PoisonError};

const APP_DIR_NAME: &str = "jellyfin-desktop";
const LOG_FILE_NAME: &str = "jellyfin-desktop.log";

#[derive(Default)]
struct Overrides {
    config_dir: Option<PathBuf>,
    cache_dir: Option<PathBuf>,
}

static OVERRIDES: OnceLock<Mutex<Overrides>> = OnceLock::new();

fn overrides() -> MutexGuard<'static, Overrides> {
    OVERRIDES
        .get_or_init(|| Mutex::new(Overrides::default()))
        .lock()
        .unwrap_or_else(PoisonError::into_inner)
}

pub fn set_config_dir_override(path: PathBuf) {
    overrides().config_dir = Some(path);
}

pub fn set_cache_dir_override(path: PathBuf) {
    overrides().cache_dir = Some(path);
}

fn config_override() -> Option<PathBuf> {
    overrides().config_dir.clone()
}

fn cache_override() -> Option<PathBuf> {
    overrides().cache_dir.clone()
}

fn env_or(var: &str, fallback: &str) -> String {
    match env::var(var) {
        Ok(v) if !v.is_empty() => v,
        _ => fallback.to_string(),
    }
}

#[cfg(not(windows))]
fn home() -> String {
    env_or("HOME", "/tmp")
}

fn ensure(path: PathBuf) -> PathBuf {
    let _ = fs::create_dir_all(&path);
    path
}

pub fn write_atomic(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let dir = path.parent().unwrap_or(Path::new("."));
    let mut tmp = tempfile::NamedTempFile::new_in(dir)?;
    tmp.write_all(bytes)?;
    tmp.as_file().sync_all()?;
    tmp.persist(path).map_err(|err| err.error)?;
    Ok(())
}

/// `Ok(false)` means another process won the race and created `path` first.
pub fn write_atomic_noclobber(path: &Path, bytes: &[u8]) -> io::Result<bool> {
    let dir = path.parent().unwrap_or(Path::new("."));
    let mut tmp = tempfile::NamedTempFile::new_in(dir)?;
    tmp.write_all(bytes)?;
    tmp.as_file().sync_all()?;
    match tmp.persist_noclobber(path) {
        Ok(_) => Ok(true),
        Err(err) if err.error.kind() == io::ErrorKind::AlreadyExists => Ok(false),
        Err(err) => Err(err.error),
    }
}

#[cfg(target_os = "linux")]
fn xdg_or_home(xdg_var: &str, home_subdir: &str) -> PathBuf {
    let fallback = format!("{}{}", home(), home_subdir);
    PathBuf::from(env_or(xdg_var, &fallback))
}

#[cfg(target_os = "linux")]
pub fn config_dir() -> PathBuf {
    if let Some(path) = config_override() {
        return ensure(path);
    }
    ensure(xdg_or_home("XDG_CONFIG_HOME", "/.config").join(APP_DIR_NAME))
}

#[cfg(target_os = "linux")]
pub fn cache_dir() -> PathBuf {
    if let Some(path) = cache_override() {
        return ensure(path);
    }
    ensure(xdg_or_home("XDG_CACHE_HOME", "/.cache").join(APP_DIR_NAME))
}

#[cfg(target_os = "linux")]
pub fn log_dir() -> PathBuf {
    ensure(xdg_or_home("XDG_STATE_HOME", "/.local/state").join(APP_DIR_NAME))
}

#[cfg(target_os = "macos")]
pub fn config_dir() -> PathBuf {
    if let Some(path) = config_override() {
        return ensure(path);
    }
    let base = env_or("XDG_CONFIG_HOME", &format!("{}/.config", home()));
    ensure(PathBuf::from(base).join(APP_DIR_NAME))
}

#[cfg(target_os = "macos")]
pub fn cache_dir() -> PathBuf {
    if let Some(path) = cache_override() {
        return ensure(path);
    }
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
    if let Some(path) = config_override() {
        return ensure(path);
    }
    ensure(PathBuf::from(env_or("APPDATA", "C:")).join(APP_DIR_NAME))
}

#[cfg(windows)]
fn local_appdata() -> String {
    env_or("LOCALAPPDATA", &env_or("APPDATA", "C:"))
}

#[cfg(windows)]
pub fn cache_dir() -> PathBuf {
    if let Some(path) = cache_override() {
        return ensure(path);
    }
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

/// Where logs go when no log file was requested explicitly. Linux: `None` —
/// stderr/journalctl is the norm. macOS/Windows: GUI processes have no
/// user-visible stderr, so default to the platform log file.
pub fn default_log_file() -> Option<PathBuf> {
    if cfg!(target_os = "linux") {
        None
    } else {
        Some(log_path())
    }
}
