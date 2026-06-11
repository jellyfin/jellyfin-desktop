use super::{APP_DIR_NAME, env_or, home};
use std::path::PathBuf;

pub(super) const DEFAULT_LOG_TO_FILE: bool = false;

fn xdg_or_home(xdg_var: &str, home_subdir: &str) -> PathBuf {
    let fallback = format!("{}{}", home(), home_subdir);
    PathBuf::from(env_or(xdg_var, &fallback))
}

pub(super) fn config_base() -> PathBuf {
    xdg_or_home("XDG_CONFIG_HOME", "/.config")
}

pub(super) fn cache_base() -> PathBuf {
    xdg_or_home("XDG_CACHE_HOME", "/.cache")
}

pub(super) fn log_dir_path() -> PathBuf {
    xdg_or_home("XDG_STATE_HOME", "/.local/state").join(APP_DIR_NAME)
}
