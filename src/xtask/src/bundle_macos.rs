use crate::paths;
use anyhow::{Context, Result, bail};
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::process::Command;

const CEF_FRAMEWORK_NAME: &str = "Chromium Embedded Framework";
const SYSTEM_PREFIXES: &[&str] = &["/usr/lib/", "/System/", "/Library/"];

// Homebrew installs dylibs as read-only and `std::fs::copy` preserves source
// permissions. The bundled copy must be writable so `install_name_tool` (and a
// re-run of this bundling step) can modify it.
fn copy_writable(src: &Path, dst: &Path) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    if dst.exists() {
        std::fs::remove_file(dst)
            .with_context(|| format!("remove_file {}", dst.display()))?;
    }
    std::fs::copy(src, dst)
        .with_context(|| format!("copy {} -> {}", src.display(), dst.display()))?;
    let mut perms = std::fs::metadata(dst)?.permissions();
    perms.set_mode(0o644);
    std::fs::set_permissions(dst, perms)
        .with_context(|| format!("chmod {}", dst.display()))?;
    Ok(())
}

pub fn complete(app: &Path) -> Result<()> {
    let macos_dir = app.join("Contents").join("MacOS");
    let fw_dir = app.join("Contents").join("Frameworks");
    let entitlements = paths::repo_root()
        .join("resources")
        .join("macos")
        .join("entitlements.plist");

    println!("App bundle: {}", app.display());

    let brew_prefix = brew_prefix()?;
    println!("Homebrew prefix: {}", brew_prefix.display());

    // CEF framework: rewrite install_name from build-tree (@executable_path/Frameworks/...)
    // to bundle-tree (@executable_path/../Frameworks/...).
    let cef_fw_lib = fw_dir
        .join(format!("{CEF_FRAMEWORK_NAME}.framework"))
        .join(CEF_FRAMEWORK_NAME);
    if cef_fw_lib.exists() {
        let old = format!(
            "@executable_path/Frameworks/{CEF_FRAMEWORK_NAME}.framework/{CEF_FRAMEWORK_NAME}"
        );
        let new = format!(
            "@executable_path/../Frameworks/{CEF_FRAMEWORK_NAME}.framework/{CEF_FRAMEWORK_NAME}"
        );
        println!("Fixing CEF framework paths...");
        install_name_tool(&["-id", &new, &cef_fw_lib.to_string_lossy()])?;
        let bin = macos_dir.join("jellyfin-desktop");
        install_name_tool(&["-change", &old, &new, &bin.to_string_lossy()])?;
    } else {
        eprintln!(
            "warning: CEF framework not found at {}",
            cef_fw_lib.display()
        );
    }

    // Bundle MoltenVK (loaded via ICD discovery, not linked).
    bundle_moltenvk(&fw_dir, &brew_prefix)?;

    // Iterative dep walk: fix all dylibs in MacOS/ + Frameworks/* + main exec.
    let mut framework_libs: HashSet<String> = std::fs::read_dir(&fw_dir)?
        .filter_map(|e| e.ok())
        .filter_map(|e| {
            let n = e.file_name().to_string_lossy().into_owned();
            n.ends_with(".dylib").then_some(n)
        })
        .collect();

    let mut queue: Vec<PathBuf> = Vec::new();
    for entry in std::fs::read_dir(&macos_dir)? {
        let p = entry?.path();
        if p.extension().and_then(|s| s.to_str()) == Some("dylib") {
            queue.push(p);
        }
    }
    queue.push(macos_dir.join("jellyfin-desktop"));

    let mut iter = 0;
    while !queue.is_empty() {
        iter += 1;
        if iter > 50 {
            bail!("dependency fix-point loop exceeded 50 iterations");
        }
        println!("=== Pass {iter} ===");
        let batch = std::mem::take(&mut queue);
        let mut seen = HashSet::new();
        for lib in batch {
            let key = lib.to_string_lossy().into_owned();
            if !seen.insert(key) || !lib.exists() {
                continue;
            }
            println!("Processing: {}", lib.file_name().unwrap().to_string_lossy());
            fix_lib_deps(&lib, &fw_dir, &brew_prefix, &mut framework_libs, &mut queue)?;
        }
    }
    println!("Library path fixing complete ({iter} passes)");

    let mut bundled: Vec<_> = std::fs::read_dir(&fw_dir)?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.ends_with(".dylib"))
        .collect();
    bundled.sort();
    println!("=== Bundled libraries ===");
    for n in &bundled {
        println!("  {n}");
    }

    // Codesign — inside-out (matches CMake script ordering exactly).
    codesign(app, &fw_dir, &macos_dir, &entitlements)?;

    // Strip quarantine.
    let _ = Command::new("xattr").args(["-cr"]).arg(app).status();

    println!("Bundle complete: {}", app.display());
    Ok(())
}

fn fix_lib_deps(
    lib: &Path,
    fw_dir: &Path,
    brew_prefix: &Path,
    framework_libs: &mut HashSet<String>,
    queue: &mut Vec<PathBuf>,
) -> Result<()> {
    let output = Command::new("otool").arg("-L").arg(lib).output()?;
    if !output.status.success() {
        return Ok(());
    }
    let text = String::from_utf8_lossy(&output.stdout);

    for raw_line in text.lines() {
        let line = raw_line.trim();
        if line.is_empty() {
            continue;
        }
        let needs_fix = line.starts_with("@rpath")
            || line.starts_with("@loader_path")
            || (line.starts_with('/') && !SYSTEM_PREFIXES.iter().any(|p| line.starts_with(p)));
        if !needs_fix {
            continue;
        }
        let dep_path = match line.split_once(" (compatibility") {
            Some((p, _)) => p.trim().to_string(),
            None => line.to_string(),
        };
        let Some(resolved) = resolve_dependency(&dep_path, brew_prefix) else {
            eprintln!("warning: could not resolve: {dep_path}");
            continue;
        };
        let dep_name = Path::new(&resolved)
            .file_name()
            .unwrap()
            .to_string_lossy()
            .into_owned();
        let target = format!("@executable_path/../Frameworks/{dep_name}");

        if !framework_libs.contains(&dep_name) {
            let real = std::fs::canonicalize(&resolved)?;
            println!("Bundling: {dep_name}");
            let dst = fw_dir.join(&dep_name);
            copy_writable(&real, &dst)?;
            install_name_tool(&["-id", &target, &dst.to_string_lossy()])?;
            framework_libs.insert(dep_name.clone());
            queue.push(dst);
        }

        println!("  Fixing: {dep_path} -> {target}");
        install_name_tool(&["-change", &dep_path, &target, &lib.to_string_lossy()])?;
    }
    Ok(())
}

fn resolve_dependency(dep_path: &str, brew_prefix: &Path) -> Option<String> {
    let brew_lib = brew_prefix.join("lib");
    if let Some(name) = dep_path.strip_prefix("@rpath/") {
        if brew_lib.join(name).exists() {
            return Some(brew_lib.join(name).to_string_lossy().into_owned());
        }
        return search_cellar(brew_prefix, name);
    }
    if let Some(name) = dep_path.strip_prefix("@loader_path/") {
        let base = Path::new(name).file_name()?.to_string_lossy().into_owned();
        if brew_lib.join(&base).exists() {
            return Some(brew_lib.join(&base).to_string_lossy().into_owned());
        }
        return search_cellar(brew_prefix, &base);
    }
    if dep_path.starts_with('/') && Path::new(dep_path).exists() {
        return Some(dep_path.to_string());
    }
    None
}

fn search_cellar(brew_prefix: &Path, name: &str) -> Option<String> {
    // Equivalent to `file(GLOB ${brew}/Cellar/*/*/lib/${name})` — take first hit.
    let cellar = brew_prefix.join("Cellar");
    let formulas = std::fs::read_dir(&cellar).ok()?;
    for f in formulas.flatten() {
        let versions = std::fs::read_dir(f.path()).ok()?;
        for v in versions.flatten() {
            let candidate = v.path().join("lib").join(name);
            if candidate.exists() {
                return Some(candidate.to_string_lossy().into_owned());
            }
        }
    }
    None
}

fn bundle_moltenvk(fw_dir: &Path, brew_prefix: &Path) -> Result<()> {
    let mut prefixes: Vec<PathBuf> = vec![brew_prefix.to_path_buf()];
    let cross_x86 = std::env::var("CMAKE_OSX_ARCHITECTURES")
        .map(|v| v == "x86_64")
        .unwrap_or(false)
        || cfg!(target_arch = "x86_64");
    if cross_x86 {
        prefixes.push(PathBuf::from("/usr/local"));
        prefixes.push(PathBuf::from("/opt/homebrew"));
    } else {
        prefixes.push(PathBuf::from("/opt/homebrew"));
        prefixes.push(PathBuf::from("/usr/local"));
    }
    prefixes.dedup();

    let mut source: Option<PathBuf> = None;
    for prefix in &prefixes {
        let direct = prefix.join("lib").join("libMoltenVK.dylib");
        if direct.exists() {
            source = Some(direct);
            break;
        }
        let cellar = prefix.join("Cellar").join("molten-vk");
        if let Ok(versions) = std::fs::read_dir(&cellar) {
            let mut cands: Vec<PathBuf> = versions
                .filter_map(|e| e.ok())
                .map(|e| e.path().join("lib").join("libMoltenVK.dylib"))
                .filter(|p| p.exists())
                .collect();
            cands.sort();
            if let Some(last) = cands.pop() {
                source = Some(last);
                break;
            }
        }
    }

    if let Some(src) = source {
        let real = std::fs::canonicalize(&src)?;
        println!("Bundling MoltenVK: {}", real.display());
        let dst = fw_dir.join("libMoltenVK.dylib");
        copy_writable(&real, &dst)?;
        install_name_tool(&[
            "-id",
            "@executable_path/../Frameworks/libMoltenVK.dylib",
            &dst.to_string_lossy(),
        ])?;
    } else {
        eprintln!("warning: MoltenVK not found - Vulkan will not work without system MoltenVK");
    }
    Ok(())
}

fn codesign(app: &Path, fw_dir: &Path, macos_dir: &Path, entitlements: &Path) -> Result<()> {
    println!("Signing app bundle...");
    let cef_fw = fw_dir.join(format!("{CEF_FRAMEWORK_NAME}.framework"));

    if cef_fw.exists() {
        // Nested dylibs inside CEF framework first.
        if let Ok(entries) = std::fs::read_dir(cef_fw.join("Libraries")) {
            for e in entries.flatten() {
                if e.path().extension().and_then(|s| s.to_str()) == Some("dylib") {
                    println!("  Signing CEF nested: {}", e.path().display());
                    sign(&e.path(), Some(entitlements))?;
                }
            }
        }
        let cef_bin = cef_fw.join(CEF_FRAMEWORK_NAME);
        println!("  Signing CEF framework binary");
        sign(&cef_bin, Some(entitlements))?;
        println!("  Signing CEF framework bundle");
        sign(&cef_fw, Some(entitlements))?;
    }

    // Other frameworks.
    for e in std::fs::read_dir(fw_dir)?.flatten() {
        let p = e.path();
        if p.extension().and_then(|s| s.to_str()) == Some("framework") && p != cef_fw {
            println!("  Signing framework: {}", p.display());
            sign(&p, Some(entitlements))?;
        }
    }

    // Loose dylibs in Frameworks/.
    for e in std::fs::read_dir(fw_dir)?.flatten() {
        let p = e.path();
        if p.extension().and_then(|s| s.to_str()) == Some("dylib") {
            println!("  Signing: {}", p.display());
            sign(&p, Some(entitlements))?;
        }
    }

    // Standalone Mach-O binaries in Frameworks/.
    for e in std::fs::read_dir(fw_dir)?.flatten() {
        let p = e.path();
        if p.is_file() && p.extension().and_then(|s| s.to_str()) != Some("dylib") {
            let ft = Command::new("file").arg(&p).output();
            if let Ok(out) = ft
                && String::from_utf8_lossy(&out.stdout).contains("Mach-O")
            {
                println!("  Signing binary: {}", p.display());
                sign(&p, Some(entitlements))?;
            }
        }
    }

    // Dylibs in MacOS/.
    for e in std::fs::read_dir(macos_dir)?.flatten() {
        let p = e.path();
        if p.extension().and_then(|s| s.to_str()) == Some("dylib") {
            println!("  Signing: {}", p.display());
            sign(&p, None)?;
        }
    }

    println!("  Signing executable with entitlements");
    sign(&macos_dir.join("jellyfin-desktop"), Some(entitlements))?;

    println!("  Signing app bundle");
    sign(app, Some(entitlements))?;
    Ok(())
}

fn sign(path: &Path, entitlements: Option<&Path>) -> Result<()> {
    let mut cmd = Command::new("codesign");
    cmd.args(["--force", "--sign", "-"]);
    if let Some(e) = entitlements {
        cmd.arg("--entitlements").arg(e);
    }
    cmd.arg(path);
    let _ = cmd.status();
    Ok(())
}

fn install_name_tool(args: &[&str]) -> Result<()> {
    let status = Command::new("install_name_tool")
        .args(args)
        .status()
        .context("spawn install_name_tool")?;
    if !status.success() {
        bail!("install_name_tool {args:?} failed");
    }
    Ok(())
}

fn brew_prefix() -> Result<PathBuf> {
    let out = Command::new("brew")
        .arg("--prefix")
        .output()
        .context("spawn brew --prefix")?;
    if !out.status.success() {
        bail!("brew --prefix failed");
    }
    Ok(PathBuf::from(String::from_utf8(out.stdout)?.trim()))
}
