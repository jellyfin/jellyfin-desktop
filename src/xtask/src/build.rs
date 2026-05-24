use crate::{BuildArgs, cef, fs as xfs, mpv, paths};
use anyhow::{Context, Result, bail};
use std::process::Command;

pub fn run(args: &BuildArgs) -> Result<()> {
    let out = std::path::absolute(&args.out)?;
    std::fs::create_dir_all(&out)?;

    let cef_info = cef::discover(&args.external_cef, args.system_cef)?;
    println!("Found CEF: {}", cef_info.version);

    let (mpv_info, used_external_mpv) = if let Some(dir) = &args.external_mpv {
        println!("Using external mpv from: {}", dir.display());
        (mpv::external(dir)?, true)
    } else {
        (mpv::build(&out, args.mpv_cli)?, false)
    };

    // Cargo invocation — mirror the env CMake passes today.
    let target_dir = paths::cargo_target_dir(&out);
    let manifest = paths::workspace_manifest();
    let mut cmd = Command::new("cargo");
    cmd.arg("build")
        .arg("--release")
        .arg("--bin")
        .arg("jellyfin-desktop")
        .arg("--manifest-path")
        .arg(&manifest);
    if args.no_kde_palette {
        cmd.arg("--no-default-features");
    }
    cmd.env("CARGO_TARGET_DIR", &target_dir);
    if let Some(dir) = &args.external_mpv {
        cmd.env("EXTERNAL_MPV_DIR", dir);
        cmd.env_remove("JFN_MPV_INCLUDE_DIR");
        cmd.env_remove("JFN_MPV_LIB_DIR");
    } else {
        cmd.env_remove("EXTERNAL_MPV_DIR");
        cmd.env("JFN_MPV_INCLUDE_DIR", paths::mpv_source_dir().join("include"));
        cmd.env("JFN_MPV_LIB_DIR", &mpv_info.build_dir);
    }

    println!("Building jellyfin-desktop (Rust binary)...");
    let status = cmd.status().context("spawn cargo build")?;
    if !status.success() {
        bail!("cargo build failed");
    }

    let bin_name = if cfg!(target_os = "windows") {
        "jellyfin-desktop.exe"
    } else {
        "jellyfin-desktop"
    };
    let bin_src = target_dir.join("release").join(bin_name);
    let bin_dst = out.join(bin_name);
    xfs::copy_file(&bin_src, &bin_dst)?;

    stage_cef(&out, &cef_info)?;
    stage_mpv(&out, &mpv_info, used_external_mpv, &bin_dst)?;
    Ok(())
}

fn stage_cef(out: &std::path::Path, cef: &cef::Cef) -> Result<()> {
    if cef.system {
        return Ok(());
    }
    if cfg!(target_os = "macos") {
        let fw_name = "Chromium Embedded Framework";
        let fw_src = cef.release_dir.join(format!("{fw_name}.framework"));
        let fw_dst = out.join("Frameworks").join(format!("{fw_name}.framework"));
        std::fs::create_dir_all(out.join("Frameworks"))?;
        if fw_dst.exists() {
            std::fs::remove_dir_all(&fw_dst)?;
        }
        xfs::copy_dir_recursive(&fw_src, &fw_dst)?;
        let new_id = format!("@executable_path/Frameworks/{fw_name}.framework/{fw_name}");
        run_install_name_tool(&[
            "-id".as_ref(),
            new_id.as_ref(),
            fw_dst.join(fw_name).as_os_str(),
        ])?;
        let old = format!("@executable_path/../Frameworks/{fw_name}.framework/{fw_name}");
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
            let target = format!("Frameworks/{fw_name}.framework/Libraries/{lib}");
            std::os::unix::fs::symlink(&target, &dst)
                .with_context(|| format!("symlink {} -> {}", dst.display(), target))?;
        }
    } else {
        xfs::copy_dir_recursive(&cef.resource_dir, out)?;
        xfs::copy_dir_recursive(&cef.release_dir, out)?;
    }
    Ok(())
}

fn stage_mpv(
    out: &std::path::Path,
    mpv_info: &mpv::Mpv,
    used_external: bool,
    bin: &std::path::Path,
) -> Result<()> {
    if !used_external {
        let runtime = mpv::runtime_library_name();
        let src = mpv_info.build_dir.join(runtime);
        let dst = out.join(runtime);
        xfs::copy_file(&src, &dst)?;
        if cfg!(target_os = "macos") {
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
    } else if cfg!(target_os = "windows") {
        let lib_dir = mpv_info.build_dir.join("lib");
        for entry in std::fs::read_dir(&lib_dir)? {
            let entry = entry?;
            let name = entry.file_name();
            let name_s = name.to_string_lossy();
            if name_s.ends_with(".dll") {
                let dst = out.join(&name);
                std::fs::copy(entry.path(), &dst)?;
            }
        }
    }
    Ok(())
}

#[cfg(unix)]
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

#[cfg(not(unix))]
fn run_install_name_tool(_args: &[&std::ffi::OsStr]) -> Result<()> {
    Ok(())
}
