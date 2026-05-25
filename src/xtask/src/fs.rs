use anyhow::{Context, Result};
use std::path::Path;

pub fn copy_file(src: &Path, dst: &Path) -> Result<()> {
    if let Some(parent) = dst.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create_dir_all {}", parent.display()))?;
    }
    std::fs::copy(src, dst)
        .with_context(|| format!("copy {} -> {}", src.display(), dst.display()))?;
    Ok(())
}

pub fn copy_dir_recursive(src: &Path, dst: &Path) -> Result<()> {
    std::fs::create_dir_all(dst).with_context(|| format!("create_dir_all {}", dst.display()))?;
    for entry in std::fs::read_dir(src).with_context(|| format!("read_dir {}", src.display()))? {
        let entry = entry?;
        let ty = entry.file_type()?;
        let src_path = entry.path();
        let dst_path = dst.join(entry.file_name());
        if ty.is_dir() {
            copy_dir_recursive(&src_path, &dst_path)?;
        } else if ty.is_symlink() {
            let _ = std::fs::remove_file(&dst_path);
            #[cfg(unix)]
            {
                let target = std::fs::read_link(&src_path)?;
                std::os::unix::fs::symlink(&target, &dst_path).with_context(|| {
                    format!("symlink {} -> {}", dst_path.display(), target.display())
                })?;
            }
            #[cfg(not(unix))]
            {
                // No symlinks expected in our staged trees on non-unix; fall back
                // to copying the resolved target so the layout still works.
                std::fs::copy(std::fs::canonicalize(&src_path)?, &dst_path).with_context(|| {
                    format!("copy {} -> {}", src_path.display(), dst_path.display())
                })?;
            }
        } else {
            std::fs::copy(&src_path, &dst_path).with_context(|| {
                format!("copy {} -> {}", src_path.display(), dst_path.display())
            })?;
        }
    }
    Ok(())
}

// Used only by the Linux/Windows install paths; the macOS bundle stages files
// individually.
#[cfg(not(target_os = "macos"))]
pub fn copy_glob(src_dir: &Path, dst_dir: &Path, patterns: &[&str]) -> Result<()> {
    std::fs::create_dir_all(dst_dir)?;
    for entry in std::fs::read_dir(src_dir)? {
        let entry = entry?;
        let name = entry.file_name().to_string_lossy().into_owned();
        if patterns.iter().any(|p| match_pattern(p, &name)) {
            let dst = dst_dir.join(&name);
            if entry.file_type()?.is_dir() {
                copy_dir_recursive(&entry.path(), &dst)?;
            } else {
                std::fs::copy(entry.path(), &dst)?;
            }
        }
    }
    Ok(())
}

#[cfg(not(target_os = "macos"))]
fn match_pattern(pat: &str, name: &str) -> bool {
    // Trivial glob: leading `*` (suffix match), trailing `*` (prefix match),
    // contains `.so` style middle match, or exact.
    if let Some(rest) = pat.strip_prefix('*') {
        if let Some(rest) = rest.strip_suffix('*') {
            name.contains(rest)
        } else {
            name.ends_with(rest)
        }
    } else if let Some(rest) = pat.strip_suffix('*') {
        name.starts_with(rest)
    } else {
        name == pat
    }
}
