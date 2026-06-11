use crate::{bundle_macos, cef, fs as xfs, mpv, paths, template, version};
use anyhow::{Context, Result, bail};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::Command;

const MACOS_APP_NAME: &str = "Jellyfin Desktop.app";
const FRAMEWORK_NAME: &str = "Chromium Embedded Framework";

pub fn stage_cef(out: &Path, cef: &cef::Cef) -> Result<()> {
    if cef.link_external {
        return Ok(());
    }
    let fw_src = cef.dir.join(format!("{FRAMEWORK_NAME}.framework"));
    let fw_dst = out
        .join("Frameworks")
        .join(format!("{FRAMEWORK_NAME}.framework"));
    std::fs::create_dir_all(out.join("Frameworks"))?;
    if fw_dst.exists() {
        std::fs::remove_dir_all(&fw_dst)?;
    }
    xfs::copy_dir_recursive(&fw_src, &fw_dst)?;
    let new_id = format!("@executable_path/Frameworks/{FRAMEWORK_NAME}.framework/{FRAMEWORK_NAME}");
    run_install_name_tool(&[
        "-id".as_ref(),
        new_id.as_ref(),
        fw_dst.join(FRAMEWORK_NAME).as_os_str(),
    ])?;
    let old = format!("@executable_path/../Frameworks/{FRAMEWORK_NAME}.framework/{FRAMEWORK_NAME}");
    let bin = out.join("jellyfin-desktop");
    run_install_name_tool(&[
        "-change".as_ref(),
        old.as_ref(),
        new_id.as_ref(),
        bin.as_os_str(),
    ])?;
    // libEGL / libGLESv2 symlinks (CEF expects them next to the binary on macOS).
    for lib in ["libEGL.dylib", "libGLESv2.dylib"] {
        let dst = out.join(lib);
        let _ = std::fs::remove_file(&dst);
        let target = format!("Frameworks/{FRAMEWORK_NAME}.framework/Libraries/{lib}");
        std::os::unix::fs::symlink(&target, &dst)
            .with_context(|| format!("symlink {} -> {}", dst.display(), target))?;
    }
    Ok(())
}

pub fn stage_mpv(out: &Path, mpv_info: &mpv::Mpv, used_external: bool, bin: &Path) -> Result<()> {
    if !used_external {
        let runtime = mpv::runtime_library_name();
        let dst = out.join(runtime);
        xfs::copy_file(&mpv_info.build_dir.join(runtime), &dst)?;
        run_install_name_tool(&[
            "-id".as_ref(),
            format!("@executable_path/{runtime}").as_ref(),
            dst.as_os_str(),
        ])?;
        run_install_name_tool(&[
            "-change".as_ref(),
            format!("@rpath/{runtime}").as_ref(),
            format!("@executable_path/{runtime}").as_ref(),
            bin.as_os_str(),
        ])?;
    }
    Ok(())
}

pub fn install(build_dir: &Path, prefix: &Path, _args: &crate::BuildArgs) -> Result<PathBuf> {
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
    xfs::copy_executable(&bin_src, &bin_dst)?;
    xfs::copy_file(
        &build_dir.join("libmpv.2.dylib"),
        &macos_dir.join("libmpv.2.dylib"),
    )?;

    // CEF framework — copy from build/Frameworks/ (already install_name-fixed for
    // build-tree layout; CompleteBundleMac re-rewrites for bundle layout).
    let fw_src = build_dir
        .join("Frameworks")
        .join(format!("{FRAMEWORK_NAME}.framework"));
    let fw_dst = fw_dir.join(format!("{FRAMEWORK_NAME}.framework"));
    if fw_dst.exists() {
        std::fs::remove_dir_all(&fw_dst)?;
    }
    xfs::copy_dir_recursive(&fw_src, &fw_dst)?;

    // libEGL / libGLESv2 symlinks (bundle layout: MacOS/ → ../Frameworks/...).
    for lib in ["libEGL.dylib", "libGLESv2.dylib"] {
        let dst = macos_dir.join(lib);
        let _ = std::fs::remove_file(&dst);
        let target = format!("../Frameworks/{FRAMEWORK_NAME}.framework/Libraries/{lib}");
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
    Ok(app)
}

fn run_install_name_tool(args: &[&std::ffi::OsStr]) -> Result<()> {
    let status = Command::new("install_name_tool")
        .args(args)
        .status()
        .context("spawn install_name_tool")?;
    if !status.success() {
        bail!("install_name_tool failed");
    }
    Ok(())
}
