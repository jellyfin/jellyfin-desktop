fn main() {
    // `cef_paths()` bakes CEF_RESOURCES_DIR via option_env!; re-run when it
    // changes so a CEF relocation isn't compiled in stale.
    println!("cargo:rerun-if-env-changed=CEF_RESOURCES_DIR");
}
