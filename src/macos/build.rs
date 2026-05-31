fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    // The crate is `#![cfg(target_os = "macos")]`; only emit the framework
    // link directives for Apple targets so the empty rlib still builds (and
    // lints) on Linux/Windows as part of `cargo --workspace`.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos") {
        println!("cargo:rustc-link-lib=framework=IOKit");
        println!("cargo:rustc-link-lib=framework=IOSurface");
        println!("cargo:rustc-link-lib=framework=Metal");
        println!("cargo:rustc-link-lib=framework=CoreGraphics");
    }
}
