use crate::{paths, version};
use anyhow::{Context, Result, anyhow, bail};
use std::path::{Path, PathBuf};

pub struct Cef {
    pub dir: PathBuf,
    pub root: PathBuf,
    pub link_external: bool,
    pub version: String,
}

fn resolve(root: &Path) -> Result<(PathBuf, PathBuf)> {
    let target = download_cef::DEFAULT_TARGET;
    let cef_version = download_cef::default_version(&version::cef_package_version()?);
    let os_arch = download_cef::OsAndArch::try_from(target).map_err(|e| anyhow!("{e}"))?;
    let versioned = root.join(&cef_version);
    let cef_dir = versioned.join(os_arch.to_string());
    Ok((versioned, cef_dir))
}

pub fn ensure(root: &Path) -> Result<PathBuf> {
    let target = download_cef::DEFAULT_TARGET;
    let cef_version = download_cef::default_version(&version::cef_package_version()?);
    let (versioned, cef_dir) = resolve(root)?;
    if cef_dir.exists() {
        return Ok(cef_dir);
    }
    let url = download_cef::default_download_url();
    let index = download_cef::CefIndex::download_from(&url).map_err(|e| anyhow!("{e}"))?;
    let platform = index.platform(target).map_err(|e| anyhow!("{e}"))?;
    let version = platform.version(&cef_version).map_err(|e| anyhow!("{e}"))?;
    let archive = version
        .download_archive_from(&url, &versioned, true)
        .map_err(|e| anyhow!("{e}"))?;
    let extracted = download_cef::extract_target_archive(target, &archive, &versioned, true)
        .map_err(|e| anyhow!("{e}"))?;
    version
        .write_archive_json(&extracted)
        .map_err(|e| anyhow!("{e}"))?;
    Ok(cef_dir)
}

pub fn discover(external: &Option<PathBuf>) -> Result<Cef> {
    let version = version::cef_package_version()?;
    let root = match external {
        Some(dir) => dir.clone(),
        None => paths::cef_cache_dir(),
    };
    let dir = ensure(&root)?;
    if !dir.exists() {
        bail!("CEF not found at {}", dir.display());
    }
    Ok(Cef {
        dir,
        root,
        link_external: false,
        version,
    })
}

pub fn explicit(dir: &Path) -> Result<Cef> {
    let version = version::cef_package_version()?;
    if !dir.exists() {
        bail!("--cef-path {} does not exist", dir.display());
    }
    let link_dir = std::fs::canonicalize(dir.join("libcef.so"))
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| dir.to_path_buf());
    Ok(Cef {
        dir: link_dir,
        root: dir.to_path_buf(),
        link_external: true,
        version,
    })
}

pub fn sdk_proxy(real_dir: &Path) -> Result<(tempfile::TempDir, PathBuf)> {
    let tmp = tempfile::tempdir()?;
    let proxy = tmp.path().to_path_buf();

    for entry in
        std::fs::read_dir(real_dir).with_context(|| format!("read {}", real_dir.display()))?
    {
        let entry = entry?;
        let name = entry.file_name();
        if name == "archive.json" {
            continue;
        }
        create_link(&entry.path(), &proxy.join(&name))
            .with_context(|| format!("link {} in cef-sdk-proxy", name.to_string_lossy()))?;
    }

    let cef_version = download_cef::default_version(&version::cef_package_version()?);
    std::fs::write(
        proxy.join("archive.json"),
        format!(r#"{{"type":"minimal","name":"cef_binary_{cef_version}","sha1":""}}"#),
    )?;

    Ok((tmp, proxy))
}

#[cfg(unix)]
fn create_link(src: &Path, dst: &Path) -> Result<()> {
    Ok(std::os::unix::fs::symlink(src, dst)?)
}

#[cfg(windows)]
fn create_link(src: &Path, dst: &Path) -> Result<()> {
    if src.is_dir() {
        std::os::windows::fs::symlink_dir(src, dst)?;
    } else {
        std::os::windows::fs::symlink_file(src, dst)?;
    }
    Ok(())
}
