use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let repo_root = manifest_dir.parent().unwrap().parent().unwrap();

    // `env!` (not std::env::var) so rustc records the dep and re-runs this
    // script when the workspace version bumps.
    println!("cargo:rerun-if-changed=../Cargo.toml");
    let version = env!("CARGO_PKG_VERSION");
    println!("cargo:rustc-env=JFN_APP_VERSION={version}");

    // VERSION_FULL = "<VERSION>+<git short hash>[-dirty]", but only for
    // pre-release VERSIONs (those with a "-suffix"); a clean release stays
    // bare. xtask injects JFN_GIT_HASH/JFN_GIT_DIRTY as the authoritative
    // source; fall back to gitoxide for a bare `cargo build`.
    println!("cargo:rerun-if-env-changed=JFN_GIT_HASH");
    println!("cargo:rerun-if-env-changed=JFN_GIT_DIRTY");
    println!("cargo:rerun-if-env-changed=CEF_RESOURCES_DIR");
    let (git_hash, dirty) = match std::env::var("JFN_GIT_HASH") {
        Ok(h) if !h.is_empty() => {
            let dirty = std::env::var("JFN_GIT_DIRTY").as_deref() == Ok("1");
            (h, dirty)
        }
        _ => git_info(repo_root),
    };
    let version_full = if !version.contains('-') || git_hash.is_empty() {
        version.to_string()
    } else if dirty {
        format!("{version}+{git_hash}-dirty")
    } else {
        format!("{version}+{git_hash}")
    };
    println!("cargo:rustc-env=JFN_APP_VERSION_FULL={version_full}");
    track_git_refs(repo_root);

    let web_dir = repo_root.join("src").join("web");
    for entry in std::fs::read_dir(&web_dir).expect("read src/web").flatten() {
        let p = entry.path();
        println!("cargo:rerun-if-changed={}", p.display());
    }
}

/// Fallback for bare `cargo build` (no xtask). Empty hash when there is no repo.
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
