use crate::paths;
use anyhow::{Context, Result, bail};
use std::path::{Path, PathBuf};
use std::process::Command;

pub struct Mpv {
    pub build_dir: PathBuf,
}

const LINK_NAME: &str = if cfg!(target_os = "macos") {
    "libmpv.dylib"
} else if cfg!(target_os = "windows") {
    "mpv.lib"
} else {
    "libmpv.so"
};

const RUNTIME_NAME: &str = if cfg!(target_os = "macos") {
    "libmpv.2.dylib"
} else if cfg!(target_os = "windows") {
    "libmpv-2.dll"
} else {
    "libmpv.so.2"
};

pub fn build(out: &Path, cplayer: bool) -> Result<Mpv> {
    let src = paths::mpv_source_dir();
    let build_dir = paths::mpv_build_dir(out);
    let cplayer_flag = if cplayer { "true" } else { "false" };

    if !build_dir.join("build.ninja").exists() {
        println!("Configuring mpv with meson (cplayer={cplayer_flag})...");
        std::fs::create_dir_all(out)?;
        let status = Command::new("meson")
            .arg("setup")
            .arg(&build_dir)
            .arg(&src)
            .arg("--default-library=shared")
            .arg("-Dlibmpv=true")
            .arg(format!("-Dcplayer={cplayer_flag}"))
            .arg("-Dlua=luajit")
            .status()
            .context("spawn meson setup")?;
        if !status.success() {
            bail!("meson setup failed");
        }
    } else {
        let status = Command::new("meson")
            .args(["configure"])
            .arg(&build_dir)
            .arg(format!("-Dcplayer={cplayer_flag}"))
            .status()
            .context("spawn meson configure")?;
        if !status.success() {
            bail!("meson configure failed");
        }
    }

    println!("Building mpv (meson handles incremental builds)...");
    let status = Command::new("meson")
        .arg("compile")
        .arg("-C")
        .arg(&build_dir)
        .status()
        .context("spawn meson compile")?;
    if !status.success() {
        bail!("meson compile failed");
    }

    let library = library_path(&build_dir);
    if !library.exists() {
        bail!("mpv library not built at {}", library.display());
    }
    let _ = library;
    Ok(Mpv { build_dir })
}

pub fn external(dir: &Path) -> Result<Mpv> {
    let library = dir.join("lib").join(LINK_NAME);
    if !library.exists() {
        bail!("mpv library not found at {}", library.display());
    }
    Ok(Mpv {
        build_dir: dir.to_path_buf(),
    })
}

pub fn library_path(build_dir: &Path) -> PathBuf {
    build_dir.join(LINK_NAME)
}

/// The shared library filename used at runtime (with SONAME).
pub fn runtime_library_name() -> &'static str {
    RUNTIME_NAME
}
