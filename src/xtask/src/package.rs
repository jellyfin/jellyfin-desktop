use crate::{PackageArgs, install, version};
use anyhow::{Context, Result};
use flate2::Compression;
use flate2::write::GzEncoder;
use std::fs::File;
use std::io::{BufWriter, Read, Seek, Write};
use std::path::Path;

struct Target {
    os_slug: &'static str,
    ext: &'static str,
}

const TARGET: Target = if cfg!(target_os = "windows") {
    Target {
        os_slug: "windows",
        ext: "zip",
    }
} else if cfg!(target_os = "macos") {
    Target {
        os_slug: "macos",
        ext: "zip",
    }
} else {
    Target {
        os_slug: "linux",
        ext: "tar.gz",
    }
};

pub fn run(args: &PackageArgs) -> Result<()> {
    let ver = version::read()?;
    let dist = std::path::absolute(&args.dist)?;
    std::fs::create_dir_all(&dist)?;

    let prefix = install::run(&args.install)?;

    let arch = current_arch();
    let name = format!("JellyfinDesktop-{}-{}-{}", ver.full, TARGET.os_slug, arch);
    let out = dist.join(format!("{name}.{}", TARGET.ext));
    let _ = std::fs::remove_file(&out);
    write_archive(&prefix, &out)?;
    println!("Wrote {}", out.display());
    Ok(())
}

fn write_archive(prefix: &Path, out: &Path) -> Result<()> {
    if cfg!(target_os = "macos") {
        // `prefix` is the .app bundle here (install::run is OS-specific); zip it
        // under its own dir name so the bundle, not its contents, is the root.
        let app_parent = prefix
            .parent()
            .with_context(|| format!("{} has no parent directory", prefix.display()))?;
        let app_dirname = prefix
            .file_name()
            .with_context(|| format!("{} has no file name", prefix.display()))?
            .to_string_lossy()
            .into_owned();
        zip_dir_with_root(app_parent, &app_dirname, out)
    } else if cfg!(target_os = "windows") {
        zip_dir(prefix, out)
    } else {
        tar_gz_dir(prefix, out)
    }
}

fn current_arch() -> &'static str {
    if cfg!(target_arch = "aarch64") {
        if cfg!(target_os = "windows") {
            "arm64"
        } else {
            "aarch64"
        }
    } else if cfg!(target_arch = "x86_64") {
        if cfg!(target_os = "windows") {
            "x64"
        } else {
            "x86_64"
        }
    } else {
        std::env::consts::ARCH
    }
}

fn tar_gz_dir(dir: &Path, out: &Path) -> Result<()> {
    let f = File::create(out).with_context(|| format!("create {}", out.display()))?;
    let gz = GzEncoder::new(BufWriter::new(f), Compression::default());
    let mut tar = tar::Builder::new(gz);
    tar.follow_symlinks(false);
    tar.append_dir_all(".", dir)?;
    tar.finish()?;
    Ok(())
}

fn zip_dir(dir: &Path, out: &Path) -> Result<()> {
    let f = File::create(out)?;
    let mut zw = zip::ZipWriter::new(BufWriter::new(f));
    let opts: zip::write::SimpleFileOptions = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    add_dir_to_zip(&mut zw, dir, Path::new(""), opts)?;
    zw.finish()?;
    Ok(())
}

fn zip_dir_with_root(parent: &Path, root_name: &str, out: &Path) -> Result<()> {
    let f = File::create(out)?;
    let mut zw = zip::ZipWriter::new(BufWriter::new(f));
    let opts: zip::write::SimpleFileOptions = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    add_dir_to_zip(&mut zw, &parent.join(root_name), Path::new(root_name), opts)?;
    zw.finish()?;
    Ok(())
}

fn add_dir_to_zip<W: Write + Seek>(
    zw: &mut zip::ZipWriter<W>,
    src: &Path,
    prefix: &Path,
    opts: zip::write::SimpleFileOptions,
) -> Result<()> {
    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let path = entry.path();
        let rel = prefix.join(entry.file_name());
        let name = rel.to_string_lossy().replace('\\', "/");
        let ft = entry.file_type()?;
        if ft.is_dir() {
            zw.add_directory(format!("{name}/"), opts)?;
            add_dir_to_zip(zw, &path, &rel, opts)?;
        } else if ft.is_symlink() {
            let target = std::fs::read_link(&path)?;
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let perm = std::fs::symlink_metadata(&path)?.permissions().mode();
                let sym_opts = opts.unix_permissions(perm);
                zw.add_symlink(name, target.to_string_lossy(), sym_opts)?;
            }
            #[cfg(not(unix))]
            {
                let _ = target;
                zw.start_file(name, opts)?;
            }
        } else {
            let mut f = File::open(&path)?;
            #[cfg(unix)]
            let file_opts = {
                use std::os::unix::fs::PermissionsExt;
                let perm = f.metadata()?.permissions().mode();
                opts.unix_permissions(perm)
            };
            #[cfg(not(unix))]
            let file_opts = opts;
            zw.start_file(name, file_opts)?;
            let mut buf = Vec::new();
            f.read_to_end(&mut buf)?;
            zw.write_all(&buf)?;
        }
    }
    Ok(())
}
