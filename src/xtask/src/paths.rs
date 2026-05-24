use std::path::PathBuf;
use std::sync::OnceLock;

pub fn repo_root() -> &'static PathBuf {
    static ROOT: OnceLock<PathBuf> = OnceLock::new();
    ROOT.get_or_init(|| {
        // src/xtask/Cargo.toml → src/xtask → src → repo_root
        let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        manifest.parent().unwrap().parent().unwrap().to_path_buf()
    })
}

pub fn workspace_manifest() -> PathBuf {
    repo_root().join("src").join("jfn_rust").join("Cargo.toml")
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
