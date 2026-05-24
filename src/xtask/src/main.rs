use anyhow::Result;
use clap::{Parser, Subcommand};
use std::path::PathBuf;

mod build;
#[cfg(target_os = "macos")]
mod bundle_macos;
mod cef;
mod fs;
mod install;
mod mpv;
mod package;
mod paths;
#[cfg(target_os = "macos")]
mod template;
mod version;

#[derive(Parser)]
#[command(name = "xtask")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    Build(BuildArgs),
    Install(InstallArgs),
    Package(PackageArgs),
}

#[derive(clap::Args, Clone)]
pub struct BuildArgs {
    /// Use the named external CEF SDK directory (must contain include/, Release/, Resources/).
    #[arg(long)]
    pub external_cef: Option<PathBuf>,
    /// Use the system-installed CEF (/usr/lib/cef, /usr/include/cef).
    #[arg(long)]
    pub system_cef: bool,
    /// Use the named external libmpv directory (must contain include/ and lib/).
    #[arg(long, env = "EXTERNAL_MPV_DIR")]
    pub external_mpv: Option<PathBuf>,
    /// Also build the standalone mpv CLI binary from the submodule.
    #[arg(long)]
    pub mpv_cli: bool,
    /// Disable the KWin per-window titlebar color feature (drops the default cargo feature).
    #[arg(long)]
    pub no_kde_palette: bool,
    /// Build directory (staged binary + runtime resources land here).
    #[arg(long, default_value = "build")]
    pub out: PathBuf,
}

#[derive(clap::Args)]
pub struct InstallArgs {
    #[command(flatten)]
    pub build: BuildArgs,
    /// Destination prefix.
    #[arg(long)]
    pub prefix: PathBuf,
    /// Skip the build step; install from an existing `--out` directory.
    #[arg(long)]
    pub skip_build: bool,
}

#[derive(clap::Args)]
pub struct PackageArgs {
    #[command(flatten)]
    pub install: InstallArgs,
    /// Output directory for the produced archive.
    #[arg(long, default_value = "dist")]
    pub dist: PathBuf,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.cmd {
        Cmd::Build(a) => build::run(&a).map(|_| ()),
        Cmd::Install(a) => install::run(&a).map(|_| ()),
        Cmd::Package(a) => package::run(&a),
    }
}
