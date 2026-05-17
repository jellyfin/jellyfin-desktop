//! Generate libmpv bindings (`mpv/client.h`) and configure linkage.
//!
//! Header source order:
//!   1. `JFN_MPV_INCLUDE_DIR` env override (set by CMake during in-tree build).
//!   2. `EXTERNAL_MPV_DIR` env override (mirrors `CMakeLists.txt:457`).
//!   3. pkg-config `mpv` (system install / `/opt/jellyfin-desktop/libmpv`).
//!   4. Vendored `third_party/mpv/include`.
//!
//! Linkage: pkg-config when available; otherwise `EXTERNAL_MPV_DIR/lib`.
//! `cargo:rustc-link-lib=mpv` always emitted.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=JFN_MPV_INCLUDE_DIR");
    println!("cargo:rerun-if-env-changed=EXTERNAL_MPV_DIR");

    let (include_dirs, linked_via_pkgconfig) = resolve_paths();
    let header = locate_header(&include_dirs);
    println!("cargo:rerun-if-changed={}", header.display());

    if !linked_via_pkgconfig {
        println!("cargo:rustc-link-lib=mpv");
    }

    let mut builder = bindgen::Builder::default()
        .header(header.to_string_lossy().to_string())
        .allowlist_function("mpv_.*")
        .allowlist_type("mpv_.*")
        .allowlist_var("MPV_.*")
        // bindgen 0.71 emits these as opaque `_address: u8` stubs because
        // they're first referenced via forward struct tags inside `mpv_node`
        // before their full typedef appears. Block the broken output and
        // hand-write correct definitions in `sys.rs`.
        .blocklist_type("mpv_node_list")
        .blocklist_type("mpv_byte_array")
        // newtype_enum: emits `pub struct mpv_foo(pub i32)` with associated
        // constants. Lets us access discriminants via `.0`, treat the enum
        // as a non-exhaustive set, and round-trip values from mpv that
        // don't match a known variant.
        .newtype_enum("mpv_event_id")
        .newtype_enum("mpv_format")
        .newtype_enum("mpv_log_level")
        .newtype_enum("mpv_error")
        .newtype_enum("mpv_end_file_reason")
        .derive_debug(true)
        .layout_tests(false)
        // mpv's client.h embeds C example code in doc comments. Carrying
        // those through as Rust doc comments breaks `cargo test` doctests,
        // so strip comments from the generated bindings.
        .generate_comments(false)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()));

    for dir in &include_dirs {
        builder = builder.clang_arg(format!("-I{}", dir.display()));
    }

    let bindings = builder
        .generate()
        .expect("failed to generate libmpv bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    bindings
        .write_to_file(&out_path)
        .expect("failed to write bindings.rs");
}

fn resolve_paths() -> (Vec<PathBuf>, bool) {
    let mut includes: Vec<PathBuf> = Vec::new();
    let mut linked = false;

    if let Ok(dir) = env::var("JFN_MPV_INCLUDE_DIR") {
        includes.push(PathBuf::from(dir));
    }

    if let Ok(dir) = env::var("EXTERNAL_MPV_DIR") {
        let root = PathBuf::from(&dir);
        includes.push(root.join("include"));
        let libdir = root.join("lib");
        println!("cargo:rustc-link-search=native={}", libdir.display());
    }

    if let Ok(lib) = pkg_config::Config::new()
        .atleast_version("0.37")
        .probe("mpv")
    {
        for p in &lib.include_paths {
            includes.push(p.clone());
        }
        linked = true;
    }

    // Vendored fallback (header-only — does not configure linkage).
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let vendored = manifest.join("../../third_party/mpv/include");
    if vendored.exists() {
        includes.push(vendored);
    }

    (includes, linked)
}

fn locate_header(include_dirs: &[PathBuf]) -> PathBuf {
    for dir in include_dirs {
        let candidate = dir.join("mpv").join("client.h");
        if candidate.exists() {
            return candidate;
        }
    }
    panic!(
        "could not locate mpv/client.h in any of: {:?}\n\
         Set JFN_MPV_INCLUDE_DIR, EXTERNAL_MPV_DIR, or install libmpv via pkg-config.",
        include_dirs
    );
}
