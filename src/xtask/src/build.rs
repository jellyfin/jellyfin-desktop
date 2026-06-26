use crate::{BuildArgs, cef, fs as xfs, mpv, paths, version};
use anyhow::{Context, Result, bail};
use std::process::Command;

pub fn run(args: &BuildArgs) -> Result<()> {
    let out = std::path::absolute(&args.out)?;
    std::fs::create_dir_all(&out)?;

    let cef_info = match &args.cef_path {
        Some(dir) => cef::explicit(dir)?,
        None => cef::discover(&args.external_cef)?,
    };
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

    let _cef_proxy;
    if cef_info.link_external {
        let (tmp, proxy) = cef::sdk_proxy(&cef_info.root)?;
        _cef_proxy = Some(tmp);
        cmd.env("CEF_PATH", &proxy);
        cmd.env("CEF_RESOURCES_DIR", &cef_info.root);
    } else {
        _cef_proxy = None;
        cmd.env("CEF_PATH", &cef_info.root);
        cmd.env_remove("CEF_RESOURCES_DIR");
    }

    // Single source of truth for the embedded commit hash. xtask always runs
    // (never cargo-cached), so it recomputes every build; the build scripts
    // read these via cargo:rerun-if-env-changed for exact invalidation.
    let (git_hash, git_dirty) = version::git_info();
    cmd.env("JFN_GIT_HASH", git_hash.unwrap_or_default());
    cmd.env("JFN_GIT_DIRTY", if git_dirty { "1" } else { "0" });

    if let Some(dir) = &args.external_mpv {
        cmd.env("EXTERNAL_MPV_DIR", dir);
        cmd.env_remove("JFN_MPV_INCLUDE_DIR");
        cmd.env_remove("JFN_MPV_LIB_DIR");
    } else {
        cmd.env_remove("EXTERNAL_MPV_DIR");
        cmd.env(
            "JFN_MPV_INCLUDE_DIR",
            paths::mpv_source_dir().join("include"),
        );
        cmd.env("JFN_MPV_LIB_DIR", &mpv_info.build_dir);
    }

    // Linux: rpath system / out-of-tree lib dirs into the binary so it
    // resolves DT_NEEDED entries that aren't shipped alongside it.
    // In-tree builds (.cache/cef + meson mpv) stay relocatable —
    // libs are staged next to the binary and $ORIGIN handles them.
    if cfg!(target_os = "linux") {
        let mut rpaths: Vec<String> = Vec::new();
        if cef_info.link_external {
            rpaths.push(cef_info.dir.to_string_lossy().into_owned());
        }
        if let Some(dir) = &args.external_mpv {
            rpaths.push(dir.join("lib").to_string_lossy().into_owned());
        }
        if rpaths.is_empty() {
            cmd.env_remove("JFN_EXTRA_RPATH");
        } else {
            cmd.env("JFN_EXTRA_RPATH", rpaths.join(":"));
        }
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

    crate::platform::stage_cef(&out, &cef_info)?;
    crate::platform::stage_mpv(&out, &mpv_info, used_external_mpv, &bin_dst)?;
    crate::platform::stage_mpv_scripts(&out)?;
    Ok(())
}
