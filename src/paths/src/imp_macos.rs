use super::{APP_DIR_NAME, env_or, home};
use std::path::PathBuf;

pub(super) const DEFAULT_LOG_TO_FILE: bool = true;

pub(super) fn config_base() -> PathBuf {
    PathBuf::from(env_or("XDG_CONFIG_HOME", &format!("{}/.config", home())))
}

pub(super) fn cache_base() -> PathBuf {
    PathBuf::from(home()).join("Library/Caches")
}

pub(super) fn log_dir_path() -> PathBuf {
    PathBuf::from(home())
        .join("Library/Logs")
        .join(APP_DIR_NAME)
}
