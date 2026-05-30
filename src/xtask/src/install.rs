use crate::{InstallArgs, fs as xfs};
#[cfg(target_os = "macos")]
use crate::{bundle_macos, paths, template, version};
#[cfg(target_os = "macos")]
use anyhow::Context;
use anyhow::{Result, bail};
#[cfg(target_os = "macos")]
use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[cfg(target_os = "macos")]
pub const MACOS_APP_NAME: &str = "Jellyfin Desktop.app";

pub fn run(args: &InstallArgs) -> Result<PathBuf> {
    let built_out = std::path::absolute(&args.build.out)?;
    if !args.skip_build {
        crate::build::run(&args.build)?;
    } else if !built_out.exists() {
        bail!(
            "--skip-build set but {} does not exist; run `cargo xtask build` first",
            built_out.display()
        );
    }
    let prefix = std::path::absolute(&args.prefix)?;
    std::fs::create_dir_all(&prefix)?;

    #[cfg(target_os = "macos")]
    {
        install_macos(&built_out, &prefix)?;
        Ok(prefix.join(MACOS_APP_NAME))
    }
    #[cfg(target_os = "windows")]
    {
        install_windows(&built_out, &prefix, &args.build)?;
        Ok(prefix)
    }
    #[cfg(all(not(target_os = "macos"), not(target_os = "windows")))]
    {
        install_linux(&built_out, &prefix, &args.build)?;
        Ok(prefix)
    }
}

#[cfg(all(not(target_os = "macos"), not(target_os = "windows")))]
fn install_linux(build_dir: &Path, prefix: &Path, args: &crate::BuildArgs) -> Result<()> {
    let bin_src = build_dir.join("jellyfin-desktop");
    let bin_dst = prefix.join("jellyfin-desktop");
    copy_executable(&bin_src, &bin_dst)?;

    if args.cef_path.is_none() {
        let cef = crate::cef::discover(&args.external_cef)?;
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
        let runtime = crate::mpv::runtime_library_name();
        xfs::copy_file(&build_dir.join(runtime), &prefix.join(runtime))?;
    }
    Ok(())
}

#[cfg(target_os = "windows")]
fn install_windows(build_dir: &Path, prefix: &Path, args: &crate::BuildArgs) -> Result<()> {
    copy_executable(
        &build_dir.join("jellyfin-desktop.exe"),
        &prefix.join("jellyfin-desktop.exe"),
    )?;
    if args.cef_path.is_none() {
        let cef = crate::cef::discover(&args.external_cef)?;
        xfs::copy_glob(&cef.dir, prefix, &["*.dll", "*.bin", "*.json"])?;
        xfs::copy_glob(&cef.dir, prefix, &["*.pak", "*.dat"])?;
        xfs::copy_dir_recursive(&cef.dir.join("locales"), &prefix.join("locales"))?;
    }
    if let Some(dir) = &args.external_mpv {
        xfs::copy_glob(&dir.join("lib"), prefix, &["*.dll"])?;
    }
    Ok(())
}

#[cfg(target_os = "macos")]
fn install_macos(build_dir: &Path, prefix: &Path) -> Result<()> {
    let app = prefix.join(MACOS_APP_NAME);
    let macos_dir = app.join("Contents").join("MacOS");
    let fw_dir = app.join("Contents").join("Frameworks");
    let resources_dir = app.join("Contents").join("Resources");
    std::fs::create_dir_all(&macos_dir)?;
    std::fs::create_dir_all(&fw_dir)?;
    std::fs::create_dir_all(&resources_dir)?;

    // Binary + staged libmpv
    let bin_src = build_dir.join("jellyfin-desktop");
    let bin_dst = macos_dir.join("jellyfin-desktop");
    copy_executable(&bin_src, &bin_dst)?;
    xfs::copy_file(
        &build_dir.join("libmpv.2.dylib"),
        &macos_dir.join("libmpv.2.dylib"),
    )?;

    // CEF framework — copy from build/Frameworks/ (already install_name-fixed for
    // build-tree layout; CompleteBundleMac re-rewrites for bundle layout).
    let fw_name = "Chromium Embedded Framework";
    let fw_src = build_dir
        .join("Frameworks")
        .join(format!("{fw_name}.framework"));
    let fw_dst = fw_dir.join(format!("{fw_name}.framework"));
    if fw_dst.exists() {
        std::fs::remove_dir_all(&fw_dst)?;
    }
    xfs::copy_dir_recursive(&fw_src, &fw_dst)?;

    // libEGL / libGLESv2 symlinks (bundle layout: MacOS/ → ../Frameworks/...).
    for lib in ["libEGL.dylib", "libGLESv2.dylib"] {
        let dst = macos_dir.join(lib);
        let _ = std::fs::remove_file(&dst);
        let target = format!("../Frameworks/{fw_name}.framework/Libraries/{lib}");
        std::os::unix::fs::symlink(&target, &dst)
            .with_context(|| format!("symlink {} -> {}", dst.display(), target))?;
    }

    // Info.plist
    let ver = version::read()?;
    let mut vars = HashMap::new();
    vars.insert("APP_VERSION_FULL", ver.full.clone());
    template::configure_file(
        &paths::repo_root()
            .join("resources")
            .join("macos")
            .join("Info.plist.in"),
        &app.join("Contents").join("Info.plist"),
        &vars,
    )?;

    // AppIcon
    let icon_src = paths::repo_root()
        .join("resources")
        .join("macos")
        .join("AppIcon.icns");
    if icon_src.exists() {
        xfs::copy_file(&icon_src, &resources_dir.join("AppIcon.icns"))?;
    }

    // MoltenVK ICD descriptor.
    let icd_dst = resources_dir
        .join("vulkan")
        .join("icd.d")
        .join("MoltenVK_icd.json");
    std::fs::create_dir_all(icd_dst.parent().unwrap())?;
    xfs::copy_file(
        &paths::repo_root()
            .join("resources")
            .join("macos")
            .join("MoltenVK_icd.json"),
        &icd_dst,
    )?;

    // Complete the bundle: dep-walk, install_name rewrites, codesign.
    bundle_macos::complete(&app)?;
    Ok(())
}

fn copy_executable(src: &Path, dst: &Path) -> Result<()> {
    xfs::copy_file(src, dst)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mut perm = std::fs::metadata(dst)?.permissions();
        perm.set_mode(0o755);
        std::fs::set_permissions(dst, perm)?;
    }
    Ok(())
}
