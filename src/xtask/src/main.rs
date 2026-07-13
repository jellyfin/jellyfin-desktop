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
#[cfg_attr(target_os = "macos", path = "platform_macos.rs")]
#[cfg_attr(target_os = "windows", path = "platform_windows.rs")]
#[cfg_attr(
    all(not(target_os = "macos"), not(target_os = "windows")),
    path = "platform_linux.rs"
)]
mod platform;
#[cfg(target_os = "macos")]
mod template;
mod test;
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
    /// Run the workspace test suite against the in-tree (or external) libmpv.
    Test(TestArgs),
    FetchCef,
    /// Print the full version string (`<semver>+<short-sha>[-dirty]`).
    Version,
}

#[derive(clap::Args, Clone)]
pub struct BuildArgs {
    /// download-cef root; staged next to the binary.
    #[arg(long)]
    pub external_cef: Option<PathBuf>,
    /// rpath an installed CEF in place.
    #[arg(long)]
    pub cef_path: Option<PathBuf>,
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
pub struct TestArgs {
    /// Use the named external libmpv directory (must contain include/ and lib/).
    #[arg(long, env = "EXTERNAL_MPV_DIR")]
    pub external_mpv: Option<PathBuf>,
    /// Build directory the mpv library was staged into (matches `build --out`).
    #[arg(long, default_value = "build")]
    pub out: PathBuf,
    /// Extra arguments forwarded verbatim to `cargo test`
    /// (e.g. `cargo xtask test -- --nocapture`).
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    pub cargo_args: Vec<String>,
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
        Cmd::Test(a) => test::run(&a),
        Cmd::FetchCef => {
            cef::ensure(&paths::cef_cache_dir()).map(|dir| println!("CEF ready: {}", dir.display()))
        }
        Cmd::Version => {
            println!("{}", version::read()?.full);
            Ok(())
        }
    }
}
