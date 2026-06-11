use super::{APP_DIR_NAME, env_or};
use std::path::PathBuf;

pub(super) const DEFAULT_LOG_TO_FILE: bool = true;

fn local_appdata() -> String {
    env_or("LOCALAPPDATA", &env_or("APPDATA", "C:"))
}

pub(super) fn config_base() -> PathBuf {
    PathBuf::from(env_or("APPDATA", "C:"))
}

pub(super) fn cache_base() -> PathBuf {
    PathBuf::from(local_appdata())
}

pub(super) fn log_dir_path() -> PathBuf {
    PathBuf::from(local_appdata())
        .join(APP_DIR_NAME)
        .join("Logs")
}
