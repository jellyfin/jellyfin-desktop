use std::path::{Path, PathBuf};
use std::sync::OnceLock;

pub fn repo_root() -> &'static PathBuf {
    static ROOT: OnceLock<PathBuf> = OnceLock::new();
    ROOT.get_or_init(|| {
        // src/xtask → src → repo_root. Falls back to the cwd, which is the
        // repo root under the usual `cargo xtask` invocation.
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(2)
            .unwrap_or_else(|| Path::new("."))
            .to_path_buf()
    })
}

pub fn workspace_manifest() -> PathBuf {
    repo_root().join("src").join("jfn_rust").join("Cargo.toml")
}

pub fn cef_cache_dir() -> PathBuf {
    repo_root().join(".cache").join("cef")
}

pub fn cargo_target_dir(out: &std::path::Path) -> PathBuf {
    out.join("cargo-target")
}

pub fn mpv_build_dir(out: &std::path::Path) -> PathBuf {
    out.join("mpv-build")
}

pub fn mpv_source_dir() -> PathBuf {
    repo_root().join("third_party").join("mpv")
}
