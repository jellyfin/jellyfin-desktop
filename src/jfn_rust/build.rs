//! Build-script hooks for the jellium-desktop binary.
//!
//! * On Windows, embed `resources/win/iconres.rc` so File Explorer
//!   surfaces the version, company, and product strings (FILE/PRODUCT
//!   version), and so the application icon shows up next to the .exe.
//!   The rc is processed by `embed-resource`, which shells out to the
//!   VS/MSVC `rc.exe` (or mingw's `windres` under non-MSVC toolchains).
//!
//! * On Windows we also hide the console: `[lib] crate-type = ["rlib"]`
//!   plus `[[bin]]` defaults to console subsystem; pass the
//!   `/SUBSYSTEM:WINDOWS` link arg so the binary launches without a
//!   spawned console window.

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("cargo:rerun-if-changed=build.rs");

    // Linux: bundle libcef.so / libmpv.so / libEGL.so etc. into a single
    // install dir alongside the binary (AppImage / flatpak / manual
    // install all follow this layout). $ORIGIN matches that and avoids
    // requiring LD_LIBRARY_PATH at runtime.
    #[cfg(all(target_os = "linux", not(target_env = "musl")))]
    {
        println!("cargo:rustc-link-arg-bins=-Wl,-rpath,$ORIGIN");
        // Permit later DT_NEEDED libraries (libcef.so) to resolve symbols
        // they don't list explicitly.
        println!("cargo:rustc-link-arg-bins=-Wl,--disable-new-dtags");
        // ELF symbol preemption needs our `wl_display_connect` interposer in the
        // dynamic symbol table to shadow libwayland's for libmpv.
        println!("cargo:rustc-link-arg-bins=-Wl,--export-dynamic");

        // Additional rpath entries for system / out-of-tree library
        // installs (e.g. Arch's `cef` package puts libcef.so in
        // /usr/lib/cef, jellium-desktop-libmpv-git in
        // /opt/jellium-desktop/libmpv/lib). xtask sets this when
        // --system-cef or --external-mpv resolves outside $ORIGIN.
        // Colon-separated; $ORIGIN entries still take precedence.
        println!("cargo:rerun-if-env-changed=JFN_EXTRA_RPATH");
        if let Ok(extra) = std::env::var("JFN_EXTRA_RPATH") {
            for entry in extra.split(':').filter(|s| !s.is_empty()) {
                println!("cargo:rustc-link-arg-bins=-Wl,-rpath,{entry}");
            }
        }
    }

    #[cfg(target_os = "windows")]
    {
        // VS_VERSION_INFO is parameterized by VERSION + git-describe —
        // expand iconres.rc.in inline (the template uses @VAR@
        // placeholders, plain textual substitution).
        use std::path::PathBuf;

        let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        let repo_root = manifest_dir
            .parent()
            .and_then(std::path::Path::parent)
            .ok_or("CARGO_MANIFEST_DIR has no grandparent")?;
        let rc_template = repo_root
            .join("resources")
            .join("win")
            .join("iconres.rc.in");
        let dpi_manifest = repo_root
            .join("resources")
            .join("win")
            .join("jellyfin-desktop.manifest");
        println!("cargo:rerun-if-changed={}", rc_template.display());
        println!("cargo:rerun-if-changed={}", dpi_manifest.display());

        let template = std::fs::read_to_string(&rc_template)?;

        // `env!` (not std::env::var) so rustc re-runs this script on a bump.
        println!("cargo:rerun-if-changed=../Cargo.toml");
        let version = env!("CARGO_PKG_VERSION").to_string();
        let numeric: Vec<&str> = version.split('-').next().unwrap_or("").split('.').collect();
        let mut major: u32 = numeric.first().and_then(|s| s.parse().ok()).unwrap_or(0);
        let mut minor: u32 = numeric.get(1).and_then(|s| s.parse().ok()).unwrap_or(0);
        let mut patch: u32 = numeric.get(2).and_then(|s| s.parse().ok()).unwrap_or(0);
        let fileflags = if version.contains('-') {
            // Zero out FILEVERSION for dev builds so they can never be
            // confused with or outrank a release on numeric comparison.
            major = 0;
            minor = 0;
            patch = 0;
            "VS_FF_PRERELEASE"
        } else {
            "0x0L"
        };
        // "<VERSION>+<short hash>[-dirty]" for pre-release VERSIONs; a clean
        // release stays bare. xtask injects JFN_GIT_HASH/JFN_GIT_DIRTY; fall
        // back to gitoxide for bare `cargo build`.
        println!("cargo:rerun-if-env-changed=JFN_GIT_HASH");
        println!("cargo:rerun-if-env-changed=JFN_GIT_DIRTY");
        let (git_hash, dirty) = match std::env::var("JFN_GIT_HASH") {
            Ok(h) if !h.is_empty() => {
                let dirty = std::env::var("JFN_GIT_DIRTY").as_deref() == Ok("1");
                (h, dirty)
            }
            _ => git_info(repo_root),
        };
        let version_full = if !version.contains('-') || git_hash.is_empty() {
            version.clone()
        } else if dirty {
            format!("{version}+{git_hash}-dirty")
        } else {
            format!("{version}+{git_hash}")
        };
        track_git_refs(repo_root);

        let cmake_source_dir = repo_root.to_string_lossy().replace('\\', "/");
        let expanded = template
            .replace("@APP_VERSION_MAJOR@", &major.to_string())
            .replace("@APP_VERSION_MINOR@", &minor.to_string())
            .replace("@APP_VERSION_PATCH@", &patch.to_string())
            .replace("@APP_VERSION_FILEFLAGS@", fileflags)
            .replace("@APP_VERSION_FULL@", &version_full)
            .replace("@CMAKE_SOURCE_DIR@", &cmake_source_dir);

        let out_dir = PathBuf::from(std::env::var("OUT_DIR")?);
        let rc_out = out_dir.join("iconres.rc");
        std::fs::write(&rc_out, expanded)?;

        embed_resource::compile(&rc_out, embed_resource::NONE).manifest_required()?;

        // Hide the console window for GUI launches. `/SUBSYSTEM:WINDOWS`
        // pairs with a `main`-style entrypoint via mainCRTStartup.
        println!("cargo:rustc-link-arg-bins=/SUBSYSTEM:WINDOWS");
        println!("cargo:rustc-link-arg-bins=/ENTRY:mainCRTStartup");
    }

    Ok(())
}

/// Fallback for bare `cargo build` (no xtask). Empty hash when there is no repo.
#[cfg(target_os = "windows")]
fn git_info(repo_root: &std::path::Path) -> (String, bool) {
    let Ok(repo) = gix::discover(repo_root) else {
        return (String::new(), false);
    };
    let hash = repo
        .head_id()
        .ok()
        .map(|id| id.to_hex_with_len(7).to_string())
        .unwrap_or_default();
    let dirty = repo.is_dirty().unwrap_or(false);
    (hash, dirty)
}

/// Re-run when HEAD moves. git_dir holds HEAD; common_dir holds refs/packed-refs
/// (they differ under a linked worktree).
#[cfg(target_os = "windows")]
fn track_git_refs(repo_root: &std::path::Path) {
    let Ok(repo) = gix::discover(repo_root) else {
        return;
    };
    println!(
        "cargo:rerun-if-changed={}",
        repo.git_dir().join("HEAD").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        repo.common_dir().join("packed-refs").display()
    );
    if let Ok(Some(r)) = repo.head_ref() {
        let name = r.name().as_bstr().to_string();
        println!(
            "cargo:rerun-if-changed={}",
            repo.common_dir().join(name).display()
        );
    }
}
