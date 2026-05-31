use crate::paths;
use anyhow::{Context, Result, anyhow};

pub struct Version {
    /// "<raw>[+<short-hash>[-dirty]]" — adds git suffix iff raw is a
    /// pre-release (has a "-suffix").
    pub full: String,
}

/// Short HEAD hash and dirty flag. `(None, false)` when there is no repo.
pub fn git_info() -> (Option<String>, bool) {
    let Ok(repo) = gix::discover(paths::repo_root()) else {
        return (None, false);
    };
    let hash = repo
        .head_id()
        .ok()
        .map(|id| id.to_hex_with_len(7).to_string());
    let dirty = repo.is_dirty().unwrap_or(false);
    (hash, dirty)
}

pub fn read() -> Result<Version> {
    let raw = env!("CARGO_PKG_VERSION").to_string();
    let full = match (raw.contains('-'), git_info()) {
        (true, (Some(hash), dirty)) => {
            let suffix = if dirty { "-dirty" } else { "" };
            format!("{raw}+{hash}{suffix}")
        }
        _ => raw,
    };
    Ok(Version { full })
}

pub fn cef_package_version() -> Result<String> {
    let lock = paths::repo_root().join("src").join("Cargo.lock");
    let lockfile =
        cargo_lock::Lockfile::load(&lock).with_context(|| format!("parse {}", lock.display()))?;
    lockfile
        .packages
        .iter()
        .find(|pkg| pkg.name.as_str() == "cef")
        .map(|pkg| pkg.version.to_string())
        .ok_or_else(|| anyhow!("`cef` package not found in {}", lock.display()))
}
