//! Argv parser for jellyfin-desktop, built on clap. `Cli::parse()` reads the
//! OS argv directly and clap owns `--help`, usage, and error rendering.
//! `--version` is the one exception: it stays a plain `bool` intercepted by
//! `app::jfn_app_main` after parsing, so the libmpv version probe (which the
//! version string needs) only fires when `--version` is actually requested,
//! not on every launch.

use clap::{ArgAction, Parser, ValueEnum};

/// Preferred paint path. A preference, not a force: the chosen tier is
/// probed and gracefully falls back down the chain dmabuf → gpu → shm.
#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum Paint {
    /// Zero-copy dmabuf shared-texture path: EGL/GBM subsurface on
    /// Wayland, Vulkan external-memory import on X11. Falls back to gpu
    /// then shm if the device can't import dmabufs.
    Dmabuf,
    /// Vulkan pixel-upload via `jfn_gpu_paint`. Falls back to shm when no
    /// Vulkan adapter is usable.
    Gpu,
    /// CPU upload (`wl_shm` / MIT-SHM). The floor of the fallback chain.
    Shm,
}

/// Display backend forced by `--platform` (Linux only).
#[cfg(target_os = "linux")]
#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum PlatformArg {
    Wayland,
    X11,
}

/// jellyfin-desktop — Jellyfin native desktop client.
///
/// The four path/logging options also read a `JELLYFIN_DESKTOP_*` environment
/// variable; an explicit flag always wins over the variable. Each option is
/// `Option<T>` with no clap default, so an absent flag/var falls back to the
/// settings.json / platform-default layer.
#[derive(Parser, Debug)]
#[command(
    name = "jellyfin-desktop",
    disable_version_flag = true,
    args_override_self = true
)]
pub struct Cli {
    /// Print version information and exit.
    #[arg(short = 'v', long, action = ArgAction::SetTrue)]
    pub version: bool,

    /// Log filter, e.g. info | debug | debug,mpv=trace,CEF=off (default: info).
    #[arg(long, env = "JELLYFIN_DESKTOP_LOG_LEVEL")]
    pub log_level: Option<String>,

    /// Write logs to this file ('' to disable).
    #[arg(long, env = "JELLYFIN_DESKTOP_LOG_FILE")]
    pub log_file: Option<String>,

    /// Override the app config directory.
    #[arg(long, env = "JELLYFIN_DESKTOP_CONFIG_DIR")]
    pub config_dir: Option<String>,

    /// Override the CEF/cache directory.
    #[arg(long, env = "JELLYFIN_DESKTOP_CACHE_DIR")]
    pub cache_dir: Option<String>,

    /// Hardware decoding mode (default: no).
    #[arg(long)]
    pub hwdec: Option<String>,

    /// Audio passthrough codecs, e.g. ac3,dts-hd,eac3,truehd.
    #[arg(long)]
    pub audio_passthrough: Option<String>,

    /// Use exclusive audio output.
    #[arg(long, action = ArgAction::SetTrue)]
    pub audio_exclusive: bool,

    /// Audio channel layout, e.g. stereo, 5.1, 7.1.
    #[arg(long)]
    pub audio_channels: Option<String>,

    /// Chrome remote debugging port.
    #[arg(long)]
    pub remote_debug_port: Option<i32>,

    /// Disable CEF GPU compositing.
    #[arg(long, action = ArgAction::SetTrue)]
    pub disable_gpu_compositing: bool,

    /// CEF ozone platform (default: follows --platform).
    #[arg(long)]
    pub ozone_platform: Option<String>,

    /// Force the display backend (Linux only).
    #[cfg(target_os = "linux")]
    #[arg(long, value_enum)]
    pub platform: Option<PlatformArg>,

    /// Preferred paint path (Linux only); falls back dmabuf→gpu→shm.
    /// Values not available on the active backend degrade gracefully.
    #[cfg(target_os = "linux")]
    #[arg(long, value_enum)]
    pub platform_paint: Option<Paint>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use clap::error::ErrorKind;
    use std::sync::Mutex;

    // clap reads process env at parse time, so env-mutating tests and the
    // tests that assert env-backed flags are unset must serialize on this.
    static ENV_LOCK: Mutex<()> = Mutex::new(());

    struct EnvGuard(&'static str);
    impl EnvGuard {
        fn set(key: &'static str, val: &str) -> Self {
            unsafe { std::env::set_var(key, val) };
            EnvGuard(key)
        }
    }
    impl Drop for EnvGuard {
        fn drop(&mut self) {
            unsafe { std::env::remove_var(self.0) };
        }
    }

    fn try_parse(args: &[&str]) -> Result<Cli, clap::Error> {
        Cli::try_parse_from(args.iter().copied())
    }

    fn ok(args: &[&str]) -> Cli {
        try_parse(args).expect("expected successful parse")
    }

    fn err_kind(args: &[&str]) -> ErrorKind {
        try_parse(args).expect_err("expected parse error").kind()
    }

    #[test]
    fn help_short() {
        assert_eq!(err_kind(&["app", "-h"]), ErrorKind::DisplayHelp);
    }

    #[test]
    fn help_long() {
        assert_eq!(err_kind(&["app", "--help"]), ErrorKind::DisplayHelp);
    }

    #[test]
    fn version_long() {
        assert!(ok(&["app", "--version"]).version);
    }

    #[test]
    fn version_short() {
        assert!(ok(&["app", "-v"]).version);
    }

    #[test]
    fn version_absent() {
        assert!(!ok(&["app"]).version);
    }

    #[test]
    fn unknown_flag() {
        assert_eq!(err_kind(&["app", "--nope"]), ErrorKind::UnknownArgument);
    }

    #[test]
    fn unknown_short_flag() {
        assert_eq!(err_kind(&["app", "-x"]), ErrorKind::UnknownArgument);
    }

    #[test]
    fn log_file_explicit_empty() {
        assert_eq!(ok(&["app", "--log-file", ""]).log_file.as_deref(), Some(""));
    }

    #[test]
    fn equals_form() {
        let a = ok(&["app", "--hwdec=vaapi", "--remote-debug-port=9222"]);
        assert_eq!(a.hwdec.as_deref(), Some("vaapi"));
        assert_eq!(a.remote_debug_port, Some(9222));
    }

    #[test]
    fn bool_flags_default_false() {
        let a = ok(&["app"]);
        assert!(!a.audio_exclusive);
        assert!(!a.disable_gpu_compositing);
    }

    #[test]
    fn bool_flags_set() {
        let a = ok(&["app", "--audio-exclusive", "--disable-gpu-compositing"]);
        assert!(a.audio_exclusive);
        assert!(a.disable_gpu_compositing);
    }

    #[test]
    fn no_args_all_unset() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let a = ok(&["app"]);
        assert!(a.hwdec.is_none());
        assert!(a.audio_passthrough.is_none());
        assert!(a.audio_channels.is_none());
        assert!(a.log_level.is_none());
        assert!(a.log_file.is_none());
        assert!(a.config_dir.is_none());
        assert!(a.cache_dir.is_none());
        assert!(a.ozone_platform.is_none());
        assert!(!a.audio_exclusive);
        assert!(!a.disable_gpu_compositing);
        assert!(a.remote_debug_port.is_none());
        #[cfg(target_os = "linux")]
        {
            assert!(a.platform.is_none());
            assert!(a.platform_paint.is_none());
        }
    }

    #[test]
    fn positional_is_error() {
        assert!(try_parse(&["app", "positional"]).is_err());
    }

    #[test]
    fn missing_trailing_value_is_error() {
        assert!(try_parse(&["app", "--log-level"]).is_err());
    }

    #[test]
    fn remote_debug_port_non_numeric_error() {
        assert!(try_parse(&["app", "--remote-debug-port=bogus"]).is_err());
    }

    #[test]
    fn space_form_common_flags() {
        let a = ok(&[
            "app",
            "--hwdec",
            "vaapi",
            "--log-level",
            "debug",
            "--log-file",
            "/tmp/x.log",
            "--config-dir",
            "/tmp/config",
            "--cache-dir",
            "/tmp/cache",
            "--audio-passthrough",
            "ac3,dts-hd",
            "--audio-channels",
            "5.1",
            "--remote-debug-port",
            "9222",
            "--ozone-platform",
            "wayland",
        ]);
        assert_eq!(a.hwdec.as_deref(), Some("vaapi"));
        assert_eq!(a.log_level.as_deref(), Some("debug"));
        assert_eq!(a.log_file.as_deref(), Some("/tmp/x.log"));
        assert_eq!(a.config_dir.as_deref(), Some("/tmp/config"));
        assert_eq!(a.cache_dir.as_deref(), Some("/tmp/cache"));
        assert_eq!(a.audio_passthrough.as_deref(), Some("ac3,dts-hd"));
        assert_eq!(a.audio_channels.as_deref(), Some("5.1"));
        assert_eq!(a.remote_debug_port, Some(9222));
        assert_eq!(a.ozone_platform.as_deref(), Some("wayland"));
    }

    #[test]
    fn log_file_unset_vs_explicit_empty() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        assert!(ok(&["app"]).log_file.is_none());
        assert_eq!(ok(&["app", "--log-file="]).log_file.as_deref(), Some(""));
    }

    #[test]
    fn prefix_collision_log_level_vs_log_file() {
        let a = ok(&["app", "--log-file=path", "--log-level=trace"]);
        assert_eq!(a.log_file.as_deref(), Some("path"));
        assert_eq!(a.log_level.as_deref(), Some("trace"));
    }

    #[test]
    fn duplicate_flag_last_wins() {
        let a = ok(&["app", "--log-level=info", "--log-level", "debug"]);
        assert_eq!(a.log_level.as_deref(), Some("debug"));
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn platform_values() {
        assert_eq!(
            ok(&["app", "--platform", "x11"]).platform,
            Some(PlatformArg::X11)
        );
        assert_eq!(
            ok(&["app", "--platform", "wayland"]).platform,
            Some(PlatformArg::Wayland)
        );
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn platform_unknown_value_errors() {
        assert_eq!(
            err_kind(&["app", "--platform=bogus"]),
            ErrorKind::InvalidValue
        );
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn paint_values() {
        assert_eq!(
            ok(&["app", "--platform-paint=dmabuf"]).platform_paint,
            Some(Paint::Dmabuf)
        );
        assert_eq!(
            ok(&["app", "--platform-paint=gpu"]).platform_paint,
            Some(Paint::Gpu)
        );
        assert_eq!(
            ok(&["app", "--platform-paint", "shm"]).platform_paint,
            Some(Paint::Shm)
        );
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn paint_unknown_value_errors() {
        assert_eq!(
            err_kind(&["app", "--platform-paint=bogus"]),
            ErrorKind::InvalidValue
        );
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn paint_last_wins() {
        assert_eq!(
            ok(&["app", "--platform-paint=dmabuf", "--platform-paint", "shm"]).platform_paint,
            Some(Paint::Shm)
        );
    }

    #[cfg(not(target_os = "linux"))]
    #[test]
    fn platform_flags_rejected_off_linux() {
        assert_eq!(
            err_kind(&["app", "--platform", "x11"]),
            ErrorKind::UnknownArgument
        );
        assert_eq!(
            err_kind(&["app", "--platform-paint", "shm"]),
            ErrorKind::UnknownArgument
        );
    }

    #[test]
    fn env_log_level_fallback() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let _v = EnvGuard::set("JELLYFIN_DESKTOP_LOG_LEVEL", "debug");
        assert_eq!(ok(&["app"]).log_level.as_deref(), Some("debug"));
    }

    #[test]
    fn env_cli_flag_overrides_env() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let _v = EnvGuard::set("JELLYFIN_DESKTOP_LOG_LEVEL", "debug");
        assert_eq!(
            ok(&["app", "--log-level", "info"]).log_level.as_deref(),
            Some("info")
        );
    }

    #[test]
    fn env_config_dir_fallback() {
        let _g = ENV_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let _v = EnvGuard::set("JELLYFIN_DESKTOP_CONFIG_DIR", "/tmp/jfd-cfg");
        assert_eq!(ok(&["app"]).config_dir.as_deref(), Some("/tmp/jfd-cfg"));
    }

    // Help text writes these defaults as literals; catch a drift in the
    // backing consts.
    #[test]
    fn const_defaults_match_help_text() {
        assert_eq!(jfn_mpv::HWDEC_DEFAULT, "no");
        assert_eq!(crate::app::DEFAULT_LOG_FILTER, "info");
    }
}
