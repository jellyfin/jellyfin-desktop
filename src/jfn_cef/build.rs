use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let repo_root = manifest_dir.parent().unwrap().parent().unwrap();

    let version_path = repo_root.join("VERSION");
    println!("cargo:rerun-if-changed={}", version_path.display());
    let version = std::fs::read_to_string(&version_path)
        .expect("read VERSION")
        .trim()
        .to_string();
    println!("cargo:rustc-env=JFN_APP_VERSION={version}");

    let cef_version_path = repo_root.join("CEF_VERSION");
    println!("cargo:rerun-if-changed={}", cef_version_path.display());
    let cef_version = std::fs::read_to_string(&cef_version_path)
        .expect("read CEF_VERSION")
        .trim()
        .to_string();
    println!("cargo:rustc-env=JFN_APP_CEF_VERSION={cef_version}");

    // VERSION_FULL = "<VERSION>+<git short hash>[-dirty]", but only for
    // pre-release VERSIONs (those with a "-suffix"); a clean release stays
    // bare. xtask injects JFN_GIT_HASH/JFN_GIT_DIRTY as the authoritative
    // source; fall back to shelling out for bare `cargo build`.
    println!("cargo:rerun-if-env-changed=JFN_GIT_HASH");
    println!("cargo:rerun-if-env-changed=JFN_GIT_DIRTY");
    let (git_hash, dirty) = match std::env::var("JFN_GIT_HASH") {
        Ok(h) if !h.is_empty() => {
            let dirty = std::env::var("JFN_GIT_DIRTY").as_deref() == Ok("1");
            (h, dirty)
        }
        _ => git_info_from_cli(repo_root),
    };
    let version_full = if !version.contains('-') || git_hash.is_empty() {
        version.clone()
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

/// Fallback for bare `cargo build` (no xtask): short hash + dirty flag.
fn git_info_from_cli(repo_root: &Path) -> (String, bool) {
    let hash = Command::new("git")
        .args(["rev-parse", "--short", "HEAD"])
        .current_dir(repo_root)
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .unwrap_or_default();
    let dirty = Command::new("git")
        .args(["status", "--porcelain"])
        .current_dir(repo_root)
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| !o.stdout.is_empty())
        .unwrap_or(false);
    (hash, dirty)
}

/// Tell cargo to re-run when HEAD moves. Resolving paths via
/// `git rev-parse --git-path` keeps this correct for worktrees,
/// `.git`-as-gitfile, and packed-refs — unlike a hardcoded `.git/HEAD`,
/// which never changes when you commit on a branch.
fn track_git_refs(repo_root: &Path) {
    let git_path = |spec: &str| -> Option<PathBuf> {
        let out = Command::new("git")
            .args(["rev-parse", "--git-path", spec])
            .current_dir(repo_root)
            .output()
            .ok()
            .filter(|o| o.status.success())?;
        let rel = String::from_utf8_lossy(&out.stdout).trim().to_string();
        if rel.is_empty() {
            return None;
        }
        let p = Path::new(&rel);
        Some(if p.is_absolute() {
            p.to_path_buf()
        } else {
            repo_root.join(p)
        })
    };

    // HEAD itself (changes on branch switch / detached checkout).
    let head = git_path("HEAD");
    if let Some(ref head) = head {
        println!("cargo:rerun-if-changed={}", head.display());
        // The ref HEAD points to is what moves on commit-on-branch.
        if let Ok(contents) = std::fs::read_to_string(head)
            && let Some(refname) = contents.strip_prefix("ref: ")
            && let Some(ref_path) = git_path(refname.trim())
        {
            println!("cargo:rerun-if-changed={}", ref_path.display());
        }
    }
    // packed-refs covers the case where the ref is packed (no loose file).
    if let Some(packed) = git_path("packed-refs") {
        println!("cargo:rerun-if-changed={}", packed.display());
    }
}
