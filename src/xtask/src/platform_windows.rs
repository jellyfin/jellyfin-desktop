use crate::{BuildArgs, cef, fs as xfs, mpv};
use anyhow::Result;
use std::path::{Path, PathBuf};

pub fn stage_cef(out: &Path, cef: &cef::Cef) -> Result<()> {
    if cef.link_external {
        return Ok(());
    }
    xfs::copy_glob(&cef.dir, out, &["*.dll", "*.bin", "*.json"])?;
    xfs::copy_glob(&cef.dir, out, &["*.pak", "*.dat"])?;
    xfs::copy_dir_recursive(&cef.dir.join("locales"), &out.join("locales"))?;
    Ok(())
}

pub fn stage_mpv_scripts(out: &Path) -> Result<()> {
    let src = crate::paths::repo_root()
        .join("resources")
        .join("mpv")
        .join("scripts");
    if src.exists() {
        xfs::copy_dir_recursive(&src, &out.join("mpv").join("scripts"))?;
    }
    Ok(())
}

pub fn stage_mpv(out: &Path, mpv_info: &mpv::Mpv, used_external: bool, _bin: &Path) -> Result<()> {
    if !used_external {
        let runtime = mpv::runtime_library_name();
        xfs::copy_file(&mpv_info.build_dir.join(runtime), &out.join(runtime))?;
    } else {
        let lib_dir = mpv_info.build_dir.join("lib");
        for entry in std::fs::read_dir(&lib_dir)? {
            let entry = entry?;
            let name = entry.file_name();
            if name.to_string_lossy().ends_with(".dll") {
                std::fs::copy(entry.path(), out.join(&name))?;
            }
        }
    }
    Ok(())
}

pub fn install(build_dir: &Path, prefix: &Path, args: &BuildArgs) -> Result<PathBuf> {
    xfs::copy_executable(
        &build_dir.join("jellium-desktop.exe"),
        &prefix.join("jellium-desktop.exe"),
    )?;
    if args.cef_path.is_none() {
        let cef = cef::discover(&args.external_cef)?;
        xfs::copy_glob(&cef.dir, prefix, &["*.dll", "*.bin", "*.json"])?;
        xfs::copy_glob(&cef.dir, prefix, &["*.pak", "*.dat"])?;
        xfs::copy_dir_recursive(&cef.dir.join("locales"), &prefix.join("locales"))?;
    }
    if let Some(dir) = &args.external_mpv {
        xfs::copy_glob(&dir.join("lib"), prefix, &["*.dll"])?;
    }
    Ok(prefix.to_path_buf())
}
