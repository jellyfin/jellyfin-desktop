use crate::InstallArgs;
use anyhow::{Result, bail};
use std::path::PathBuf;

pub fn run(args: &InstallArgs) -> Result<PathBuf> {
    let built_out = std::path::absolute(&args.build.out)?;
    if !args.skip_build {
        crate::build::run(&args.build)?;
    } else if !built_out.exists() {
        bail!(
            "--skip-build set but {} does not exist; run `cargo xtask build` first",
            built_out.display()
        );
    }
    let prefix = std::path::absolute(&args.prefix)?;
    std::fs::create_dir_all(&prefix)?;
    crate::platform::install(&built_out, &prefix, &args.build)
}
