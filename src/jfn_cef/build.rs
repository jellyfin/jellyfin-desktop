use std::path::PathBuf;
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

    // VERSION_FULL = "<VERSION>+<git short hash>[-dirty]".
    let git_hash = Command::new("git")
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
    let version_full = if git_hash.is_empty() {
        version.clone()
    } else if dirty {
        format!("{version}+{git_hash}-dirty")
    } else {
        format!("{version}+{git_hash}")
    };
    println!("cargo:rustc-env=JFN_APP_VERSION_FULL={version_full}");
    println!(
        "cargo:rerun-if-changed={}",
        repo_root.join(".git/HEAD").display()
    );

    let web_dir = repo_root.join("src").join("web");
    for entry in std::fs::read_dir(&web_dir).expect("read src/web").flatten() {
        let p = entry.path();
        println!("cargo:rerun-if-changed={}", p.display());
    }
}
