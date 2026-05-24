use crate::{paths, version};
use anyhow::{Context, Result, bail};
use regex::Regex;
use std::path::{Path, PathBuf};

pub struct Cef {
    pub resource_dir: PathBuf,
    pub release_dir: PathBuf,
    pub system: bool,
    pub version: String,
}

pub fn discover(external: &Option<PathBuf>, system: bool) -> Result<Cef> {
    if let Some(dir) = external {
        let header = dir.join("include").join("cef_version.h");
        let v = read_cef_version(&header)?;
        version::check_cef_version(&v)?;
        return Ok(Cef {
            resource_dir: dir.join("lib"),
            release_dir: dir.join("lib"),
            system: false,
            version: v,
        });
    }
    if system {
        let header = Path::new("/usr/include/cef/include/cef_version.h");
        if !header.exists() {
            bail!("--system-cef requested but {} not found", header.display());
        }
        let v = read_cef_version(header)?;
        version::warn_cef_version(&v)?;
        return Ok(Cef {
            resource_dir: PathBuf::from("/usr/lib/cef"),
            release_dir: PathBuf::from("/usr/lib/cef"),
            system: true,
            version: v,
        });
    }
    let third_party = paths::repo_root().join("third_party").join("cef");
    let header = third_party.join("include").join("cef_version.h");
    if header.exists() {
        let v = read_cef_version(&header)?;
        version::check_cef_version(&v)?;
        return Ok(Cef {
            resource_dir: third_party.join("Resources"),
            release_dir: third_party.join("Release"),
            system: false,
            version: v,
        });
    }
    bail!(
        "CEF not found. Pass --external-cef DIR, --system-cef, or populate third_party/cef (dev/tools/download_cef.py)."
    )
}

fn read_cef_version(header: &Path) -> Result<String> {
    let content =
        std::fs::read_to_string(header).with_context(|| format!("read {}", header.display()))?;
    let re = Regex::new(r#"CEF_VERSION "([^"]+)""#).unwrap();
    let caps = re
        .captures(&content)
        .ok_or_else(|| anyhow::anyhow!("CEF_VERSION macro not found in {}", header.display()))?;
    Ok(caps[1].to_string())
}
