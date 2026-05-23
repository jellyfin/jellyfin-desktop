//! Build-script hooks for the jellyfin-desktop binary.
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

fn main() {
    println!("cargo:rerun-if-changed=build.rs");

    // Linux: bundle libcef.so / libmpv.so / libEGL.so etc. into a single
    // install dir alongside the binary (AppImage / flatpak / manual
    // install all follow this layout). $ORIGIN matches that and avoids
    // requiring LD_LIBRARY_PATH at runtime.
    #[cfg(all(target_os = "linux", not(target_env = "musl")))]
    {
        println!("cargo:rustc-link-arg-bins=-Wl,-rpath,$ORIGIN");
        // Permit later DT_NEEDED libraries (libcef.so) to resolve symbols
        // they don't list explicitly — matches the prior C++ ld defaults.
        println!("cargo:rustc-link-arg-bins=-Wl,--disable-new-dtags");
    }

    #[cfg(target_os = "windows")]
    {
        // VS_VERSION_INFO is parameterized by the values produced from
        // /VERSION and git-describe — we expand iconres.rc.in inline here
        // (the file uses CMake @VAR@ placeholders so we do a simple
        // textual substitution rather than re-running cmake just for
        // this).
        use std::path::PathBuf;

        let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        let repo_root = manifest_dir.parent().unwrap().parent().unwrap();
        let rc_template = repo_root.join("resources").join("win").join("iconres.rc.in");
        println!("cargo:rerun-if-changed={}", rc_template.display());

        let template = std::fs::read_to_string(&rc_template)
            .expect("read resources/win/iconres.rc.in");

        let version = std::fs::read_to_string(repo_root.join("VERSION"))
            .expect("read VERSION")
            .trim()
            .to_string();
        let numeric: Vec<&str> = version
            .splitn(2, '-')
            .next()
            .unwrap()
            .split('.')
            .collect();
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
        let git_hash = std::process::Command::new("git")
            .args(["describe", "--always", "--dirty"])
            .current_dir(repo_root)
            .output()
            .ok()
            .filter(|o| o.status.success())
            .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
            .unwrap_or_default();
        let version_full = if !version.contains('-') || git_hash.is_empty() {
            version.clone()
        } else {
            format!("{version}+{git_hash}")
        };

        let cmake_source_dir = repo_root.to_string_lossy().replace('\\', "/");
        let expanded = template
            .replace("@APP_VERSION_MAJOR@", &major.to_string())
            .replace("@APP_VERSION_MINOR@", &minor.to_string())
            .replace("@APP_VERSION_PATCH@", &patch.to_string())
            .replace("@APP_VERSION_FILEFLAGS@", fileflags)
            .replace("@APP_VERSION_FULL@", &version_full)
            .replace("@CMAKE_SOURCE_DIR@", &cmake_source_dir);

        let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
        let rc_out = out_dir.join("iconres.rc");
        std::fs::write(&rc_out, expanded).expect("write iconres.rc");

        embed_resource::compile(&rc_out, embed_resource::NONE)
            .manifest_required()
            .expect("embed iconres.rc");

        // Hide the console window for GUI launches. `/SUBSYSTEM:WINDOWS`
        // pairs with a `main`-style entrypoint via mainCRTStartup.
        println!("cargo:rustc-link-arg-bins=/SUBSYSTEM:WINDOWS");
        println!("cargo:rustc-link-arg-bins=/ENTRY:mainCRTStartup");
    }
}
