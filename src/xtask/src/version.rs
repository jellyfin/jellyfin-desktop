use crate::paths;
use anyhow::{Context, Result, anyhow, bail};
use regex::Regex;
use std::process::Command;

pub struct Version {
    /// "<raw>[+<git-describe>]" — adds git suffix iff raw has a "-suffix".
    pub full: String,
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
    let full = if raw.contains('-') {
        let hash = Command::new("git")
            .args(["describe", "--always", "--dirty"])
            .current_dir(paths::repo_root())
            .output()
            .ok()
            .filter(|o| o.status.success())
            .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
            .unwrap_or_default();
        if hash.is_empty() {
            raw
        } else {
            format!("{raw}+{hash}")
        }
    } else {
        raw
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
