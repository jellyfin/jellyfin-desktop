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

    generate_avcodec_bindings();
}

/// Narrow libavcodec bindings: only the four symbols `capabilities`
/// needs (`av_codec_iterate`, `av_codec_is_decoder`, `avcodec_get_name`)
/// plus the `AVCodec` / `AVCodecID` / `AVMediaType` types they reference.
///
/// Header source order:
///   1. `EXTERNAL_AVCODEC_DIR` env override.
///   2. `EXTERNAL_MPV_DIR` env override (Windows ships ffmpeg headers
///      under the same prefix — see `dev/windows/build_mpv_source.ps1`).
///   3. pkg-config `libavcodec`.
fn generate_avcodec_bindings() {
    println!("cargo:rerun-if-env-changed=EXTERNAL_AVCODEC_DIR");

    let mut include_dirs: Vec<PathBuf> = Vec::new();
    let mut linked_via_pkgconfig = false;

    if let Ok(dir) = env::var("EXTERNAL_AVCODEC_DIR") {
        let root = PathBuf::from(&dir);
        include_dirs.push(root.join("include"));
        let libdir = root.join("lib");
        println!("cargo:rustc-link-search=native={}", libdir.display());
        println!("cargo:rustc-link-lib=avcodec");
    } else if let Ok(dir) = env::var("EXTERNAL_MPV_DIR") {
        // Windows ps1 copies libavcodec/libavutil headers next to mpv
        // headers and emits an avcodec.lib import library alongside
        // mpv.lib. Detect that layout and reuse it.
        let root = PathBuf::from(&dir);
        let candidate = root.join("include").join("libavcodec").join("avcodec.h");
        if candidate.exists() {
            include_dirs.push(root.join("include"));
            let libdir = root.join("lib");
            println!("cargo:rustc-link-search=native={}", libdir.display());
            println!("cargo:rustc-link-lib=avcodec");
        }
    }

    if include_dirs.is_empty() {
        let lib = pkg_config::Config::new()
            .probe("libavcodec")
            .expect("libavcodec via pkg-config");
        include_dirs.extend(lib.include_paths.iter().cloned());
        linked_via_pkgconfig = true;
    }
    let _ = linked_via_pkgconfig;

    let header_dir = include_dirs
        .iter()
        .find(|p| p.join("libavcodec/avcodec.h").exists())
        .cloned()
        .unwrap_or_else(|| {
            panic!(
                "could not locate libavcodec/avcodec.h in any of: {:?}\n\
                 Set EXTERNAL_AVCODEC_DIR, EXTERNAL_MPV_DIR (with ffmpeg \
                 headers under include/), or install libavcodec via pkg-config.",
                include_dirs
            )
        });
    let header = header_dir.join("libavcodec/avcodec.h");
    println!("cargo:rerun-if-changed={}", header.display());

    let mut builder = bindgen::Builder::default()
        .header(header.to_string_lossy().to_string())
        .allowlist_function("av_codec_iterate")
        .allowlist_function("av_codec_is_decoder")
        .allowlist_function("avcodec_get_name")
        .allowlist_type("AVCodec")
        .allowlist_type("AVCodecID")
        .allowlist_type("AVMediaType")
        .newtype_enum("AVMediaType")
        .newtype_enum("AVCodecID")
        .derive_debug(true)
        .layout_tests(false)
        .generate_comments(false)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()));

    for dir in &include_dirs {
        builder = builder.clang_arg(format!("-I{}", dir.display()));
    }

    let bindings = builder
        .generate()
        .expect("failed to generate libavcodec bindings");

    let out_path =
        PathBuf::from(env::var("OUT_DIR").unwrap()).join("avcodec_bindings.rs");
    bindings
        .write_to_file(&out_path)
        .expect("failed to write avcodec_bindings.rs");
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
