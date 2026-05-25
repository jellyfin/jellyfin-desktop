use crate::paths;
use anyhow::{Context, Result, anyhow, bail};
use regex::Regex;
use std::process::Command;

pub struct Version {
    /// "<raw>[+<short-hash>[-dirty]]" — adds git suffix iff raw is a
    /// pre-release (has a "-suffix").
    pub full: String,
}

/// Resolve the current commit's short hash and whether the working tree is
/// dirty. Returns `(None, _)` when git is unavailable (e.g. no `.git`).
pub fn git_info() -> (Option<String>, bool) {
    let hash = Command::new("git")
        .args(["rev-parse", "--short", "HEAD"])
        .current_dir(paths::repo_root())
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .filter(|s| !s.is_empty());
    let dirty = Command::new("git")
        .args(["status", "--porcelain"])
        .current_dir(paths::repo_root())
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| !o.stdout.is_empty())
        .unwrap_or(false);
    (hash, dirty)
}

pub fn read() -> Result<Version> {
    let path = paths::repo_root().join("VERSION");
    let raw = std::fs::read_to_string(&path)
        .with_context(|| format!("read {}", path.display()))?
        .trim()
        .to_string();
    let re = Regex::new(r"^(\d+)\.(\d+)\.(\d+)(?:-.*)?$").unwrap();
    if !re.is_match(&raw) {
        return Err(anyhow!(
            "VERSION must be MAJOR.MINOR.PATCH[-suffix]; got '{raw}'"
        ));
    }
    // Only pre-release builds carry the commit suffix; a clean release
    // VERSION stays bare.
    let full = match (raw.contains('-'), git_info()) {
        (true, (Some(hash), dirty)) => {
            let suffix = if dirty { "-dirty" } else { "" };
            format!("{raw}+{hash}{suffix}")
        }
        _ => raw,
    };
    Ok(Version { full })
}

pub fn check_cef_version(found: &str) -> Result<()> {
    let path = paths::repo_root().join("CEF_VERSION");
    if !path.exists() {
        return Ok(());
    }
    let expected = std::fs::read_to_string(&path)?.trim().to_string();
    if expected != found {
        bail!("CEF version mismatch:\n  CEF_VERSION file: {expected}\n  Found CEF:        {found}");
    }
    Ok(())
}

pub fn warn_cef_version(found: &str) -> Result<()> {
    let path = paths::repo_root().join("CEF_VERSION");
    if !path.exists() {
        return Ok(());
    }
    let expected = std::fs::read_to_string(&path)?.trim().to_string();
    if expected != found {
        eprintln!(
            "warning: system CEF version does not match CEF_VERSION file:\n  CEF_VERSION file: {expected}\n  System CEF:       {found}"
        );
    }
    Ok(())
}
