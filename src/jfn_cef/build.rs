use std::path::PathBuf;

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

    let web_dir = repo_root.join("src").join("web");
    for entry in std::fs::read_dir(&web_dir).expect("read src/web").flatten() {
        let p = entry.path();
        if p.extension().and_then(|s| s.to_str()) == Some("js") {
            println!("cargo:rerun-if-changed={}", p.display());
        }
    }
}
