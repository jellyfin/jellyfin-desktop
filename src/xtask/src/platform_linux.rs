use crate::{BuildArgs, cef, fs as xfs, mpv};
use anyhow::Result;
use std::path::{Path, PathBuf};

pub fn stage_cef(out: &Path, cef: &cef::Cef) -> Result<()> {
    if cef.link_external {
        return Ok(());
    }
    xfs::copy_glob(&cef.dir, out, &["*.so*", "*.bin"])?;
    xfs::copy_glob(&cef.dir, out, &["*.pak", "*.dat"])?;
    xfs::copy_dir_recursive(&cef.dir.join("locales"), &out.join("locales"))?;
    Ok(())
}

pub fn stage_mpv_scripts(_out: &Path) -> Result<()> {
    Ok(())
}

pub fn stage_mpv(out: &Path, mpv_info: &mpv::Mpv, used_external: bool, _bin: &Path) -> Result<()> {
    if !used_external {
        let runtime = mpv::runtime_library_name();
        xfs::copy_file(&mpv_info.build_dir.join(runtime), &out.join(runtime))?;
    }
    Ok(())
}

pub fn install(build_dir: &Path, prefix: &Path, args: &BuildArgs) -> Result<PathBuf> {
    let bin_src = build_dir.join("jellium-desktop");
    let bin_dst = prefix.join("jellium-desktop");
    xfs::copy_executable(&bin_src, &bin_dst)?;

    if args.cef_path.is_none() {
        let cef = cef::discover(&args.external_cef)?;
        xfs::copy_glob(&cef.dir, prefix, &["*.so*", "*.bin"])?;
        xfs::copy_glob(&cef.dir, prefix, &["*.pak", "*.dat"])?;
        xfs::copy_dir_recursive(&cef.dir.join("locales"), &prefix.join("locales"))?;
    }

    if let Some(dir) = &args.external_mpv {
        xfs::copy_file(
            &dir.join("lib").join("libmpv.so"),
            &prefix.join("libmpv.so"),
        )?;
    } else {
        let runtime = mpv::runtime_library_name();
        xfs::copy_file(&build_dir.join(runtime), &prefix.join(runtime))?;
    }
    Ok(prefix.to_path_buf())
}
