use crate::{TestArgs, mpv, paths};
use anyhow::{Context, Result, bail};
#[cfg(not(target_os = "windows"))]
use std::path::Path;
use std::process::Command;

/// Run the workspace test suite against the in-tree (or external) libmpv.
///
/// Plain `cargo test` can't link or load libmpv on its own: the `jfn-mpv`
/// build script only emits the `rustc-link-search` for libmpv when
/// `JFN_MPV_LIB_DIR` (or `EXTERNAL_MPV_DIR`) is set, and the resulting test
/// binaries reference the dylib by its install name (`@rpath/libmpv.2.dylib`
/// on macOS, `libmpv.so.2` on Linux) without ever being staged next to it the
/// way the app bundle is. This mirrors the env `build` passes to `cargo build`
/// and bakes an rpath to the mpv lib dir into every test binary so it loads.
///
/// mpv is *located*, not rebuilt: the `test` just-recipe depends on `build`,
/// and reconfiguring the meson build here would thrash its `cplayer` flag.
pub fn run(args: &TestArgs) -> Result<()> {
    let out = std::path::absolute(&args.out)?;

    let (lib_dir, external) = if let Some(dir) = &args.external_mpv {
        let dir = std::path::absolute(dir)?;
        // Validates that lib/<mpv library> exists under the external dir.
        mpv::external(&dir)?;
        (dir.join("lib"), true)
    } else {
        let build_dir = paths::mpv_build_dir(&out);
        if !mpv::library_path(&build_dir).exists() {
            bail!(
                "mpv library not found at {} — run `just build` (or \
                 `cargo xtask build`) before testing",
                build_dir.display()
            );
        }
        (build_dir, false)
    };

    let manifest = paths::repo_root().join("src").join("Cargo.toml");
    let mut cmd = Command::new("cargo");
    cmd.arg("test")
        .arg("--manifest-path")
        .arg(&manifest)
        .arg("--workspace")
        .args(&args.cargo_args);

    if external {
        if let Some(dir) = &args.external_mpv {
            cmd.env("EXTERNAL_MPV_DIR", std::path::absolute(dir)?);
        }
        cmd.env_remove("JFN_MPV_INCLUDE_DIR");
        cmd.env_remove("JFN_MPV_LIB_DIR");
    } else {
        cmd.env_remove("EXTERNAL_MPV_DIR");
        cmd.env(
            "JFN_MPV_INCLUDE_DIR",
            paths::mpv_source_dir().join("include"),
        );
        cmd.env("JFN_MPV_LIB_DIR", &lib_dir);
    }

    // Windows resolves the import library + DLL differently and needs no rpath.
    #[cfg(not(target_os = "windows"))]
    add_lib_rpath(&mut cmd, &lib_dir);

    println!("Running workspace tests...");
    if !cmd.status().context("spawn cargo test")?.success() {
        bail!("cargo test failed");
    }
    Ok(())
}

/// Append `-C link-arg=-Wl,-rpath,<lib_dir>` to the child cargo's rustflags,
/// preserving any the caller already set. Uses `CARGO_ENCODED_RUSTFLAGS` (the
/// `0x1f`-separated form) so a lib dir containing spaces survives, and clears
/// `RUSTFLAGS` since cargo rejects both being set at once.
#[cfg(not(target_os = "windows"))]
fn add_lib_rpath(cmd: &mut Command, lib_dir: &Path) {
    let flag = format!("-Clink-arg=-Wl,-rpath,{}", lib_dir.display());
    let existing = std::env::var("CARGO_ENCODED_RUSTFLAGS").ok().or_else(|| {
        std::env::var("RUSTFLAGS")
            .ok()
            .map(|s| s.split_whitespace().collect::<Vec<_>>().join("\u{1f}"))
    });
    let encoded = match existing {
        Some(prev) if !prev.is_empty() => format!("{prev}\u{1f}{flag}"),
        _ => flag,
    };
    cmd.env("CARGO_ENCODED_RUSTFLAGS", encoded);
    cmd.env_remove("RUSTFLAGS");
}
