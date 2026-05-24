fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rustc-link-lib=framework=IOKit");
    println!("cargo:rustc-link-lib=framework=IOSurface");
    println!("cargo:rustc-link-lib=framework=Metal");
    println!("cargo:rustc-link-lib=framework=CoreGraphics");
}
