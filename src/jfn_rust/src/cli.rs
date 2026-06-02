//! Argv parser for jellyfin-desktop, built on clap. Help/version
//! short-circuits return distinct variants so the caller can run its own
//! printers (which need a live `mpv_handle` and version macros not
//! available here).

use clap::error::ContextKind;
use clap::{ArgAction, Parser, ValueEnum};

/// Force the X11 paint path, bypassing the Vulkan probe.
#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum X11Paint {
    /// Vulkan pixel-upload via `jfn_gpu_paint`. Hard-fails init when no
    /// Vulkan adapter is usable.
    Gpu,
    /// MIT-SHM CPU upload. Skips Vulkan init entirely.
    Shm,
}

/// Force the Wayland paint path, bypassing the EGL/GBM dmabuf probe.
#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum WaylandPaint {
    /// EGL/GBM dmabuf shared-texture path. If the dmabuf path is truly
    /// broken at runtime, CEF surfaces the error itself.
    Dmabuf,
    /// Vulkan-WSI pixel-upload via `jfn_gpu_paint`. Disables CEF
    /// shared-texture and presents BGRA frames through
    /// `VK_KHR_wayland_surface`. Hard-fails init when no Vulkan
    /// adapter is usable.
    Gpu,
    /// `wl_shm` CPU upload. Calls `set_shared_texture_unsupported`
    /// immediately.
    Shm,
}

/// Parsed flags carried by [`CliOutcome::Continue`]. Each optional value is
/// `None` when the flag was absent; `log_file` is `Some("")` when passed
/// explicitly empty (`--log-file ''`) so the caller can tell "unset" from
/// "set to empty".
#[derive(Debug, Default)]
pub struct CliArgs {
    pub hwdec: Option<String>,
    pub audio_passthrough: Option<String>,
    pub audio_channels: Option<String>,
    pub log_level: Option<String>,
    pub log_file: Option<String>,
    pub config_dir: Option<String>,
    pub cache_dir: Option<String>,
    pub ozone_platform: Option<String>,
    // Read only on Linux (display-backend selection); compiles out elsewhere.
    #[cfg_attr(not(target_os = "linux"), allow(dead_code))]
    pub platform_override: Option<String>,
    pub audio_exclusive: bool,
    pub disable_gpu_compositing: bool,
    pub remote_debugging_port: Option<i32>,
    #[cfg_attr(not(target_os = "linux"), allow(dead_code))]
    pub x11_paint: Option<X11Paint>,
    #[cfg_attr(not(target_os = "linux"), allow(dead_code))]
    pub wayland_paint: Option<WaylandPaint>,
}

/// Result of parsing argv.
pub enum CliOutcome {
    Continue(Box<CliArgs>),
    Help,
    Version,
    /// Parse failed; carries the offending argument (empty if clap gave none).
    Error(String),
}

#[derive(Parser, Debug)]
#[command(disable_help_flag = true, disable_version_flag = true)]
struct Cli {
    #[arg(short = 'h', long, action = ArgAction::SetTrue)]
    help: bool,
    #[arg(short = 'v', long, action = ArgAction::SetTrue)]
    version: bool,
    #[arg(long, overrides_with = "log_level")]
    log_level: Option<String>,
    #[arg(long, overrides_with = "log_file")]
    log_file: Option<String>,
    #[arg(long, overrides_with = "config_dir")]
    config_dir: Option<String>,
    #[arg(long, overrides_with = "cache_dir")]
    cache_dir: Option<String>,
    #[arg(long, overrides_with = "hwdec")]
    hwdec: Option<String>,
    #[arg(long, overrides_with = "audio_passthrough")]
    audio_passthrough: Option<String>,
    // Count so we can tell "absent" (0) from "present" (>=1) without losing
    // the bool semantics.
    #[arg(long, action = ArgAction::Count)]
    audio_exclusive: u8,
    #[arg(long, overrides_with = "audio_channels")]
    audio_channels: Option<String>,
    #[arg(long, overrides_with = "remote_debug_port")]
    remote_debug_port: Option<i32>,
    #[arg(long, action = ArgAction::Count)]
    disable_gpu_compositing: u8,
    #[arg(long, overrides_with = "ozone_platform")]
    ozone_platform: Option<String>,
    #[arg(long, overrides_with = "platform")]
    platform: Option<String>,
    #[arg(long, value_enum, overrides_with = "x11_paint")]
    x11_paint: Option<X11Paint>,
    #[arg(long, value_enum, overrides_with = "wayland_paint")]
    wayland_paint: Option<WaylandPaint>,
}

/// Parse `args` (argv, including argv[0]). `have_x11` gates `--platform`,
/// which only exists on Linux builds with the x11 backend.
pub fn parse(args: Vec<String>, have_x11: bool) -> CliOutcome {
    match Cli::try_parse_from(args) {
        Ok(cli) => {
            if cli.help {
                return CliOutcome::Help;
            }
            if cli.version {
                return CliOutcome::Version;
            }
            if cli.platform.is_some() && !have_x11 {
                return CliOutcome::Error("--platform".to_string());
            }
            CliOutcome::Continue(Box::new(CliArgs {
                hwdec: cli.hwdec,
                audio_passthrough: cli.audio_passthrough,
                audio_channels: cli.audio_channels,
                log_level: cli.log_level,
                log_file: cli.log_file,
                config_dir: cli.config_dir,
                cache_dir: cli.cache_dir,
                ozone_platform: cli.ozone_platform,
                platform_override: cli.platform,
                audio_exclusive: cli.audio_exclusive > 0,
                disable_gpu_compositing: cli.disable_gpu_compositing > 0,
                remote_debugging_port: cli.remote_debug_port,
                x11_paint: cli.x11_paint,
                wayland_paint: cli.wayland_paint,
            }))
        }
        Err(err) => {
            let bad = err.context().find_map(|(k, v)| match k {
                ContextKind::InvalidArg => Some(v.to_string()),
                _ => None,
            });
            // clap renders missing-value errors as "--flag <VALUE_NAME>";
            // strip the placeholder so the reported arg is just the flag.
            let bad = bad
                .as_deref()
                .map(|s| s.split_whitespace().next().unwrap_or(s).to_string())
                .unwrap_or_default();
            CliOutcome::Error(bad)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse_args(args: &[&str]) -> CliOutcome {
        parse(args.iter().map(|s| s.to_string()).collect(), true)
    }

    fn cont(args: &[&str]) -> CliArgs {
        match parse_args(args) {
            CliOutcome::Continue(a) => *a,
            _ => panic!("expected Continue"),
        }
    }

    #[test]
    fn help_short() {
        assert!(matches!(parse_args(&["app", "-h"]), CliOutcome::Help));
    }

    #[test]
    fn help_long() {
        assert!(matches!(parse_args(&["app", "--help"]), CliOutcome::Help));
    }

    #[test]
    fn version_long() {
        assert!(matches!(
            parse_args(&["app", "--version"]),
            CliOutcome::Version
        ));
    }

    #[test]
    fn version_short() {
        assert!(matches!(parse_args(&["app", "-v"]), CliOutcome::Version));
    }

    #[test]
    fn unknown_flag() {
        match parse_args(&["app", "--nope"]) {
            CliOutcome::Error(s) => assert!(s.contains("--nope")),
            _ => panic!("expected Error"),
        }
    }

    #[test]
    fn unknown_short_flag() {
        match parse_args(&["app", "-x"]) {
            CliOutcome::Error(s) => assert!(s.contains("-x")),
            _ => panic!("expected Error"),
        }
    }

    #[test]
    fn log_file_explicit_empty() {
        let a = cont(&["app", "--log-file", ""]);
        assert_eq!(a.log_file.as_deref(), Some(""));
    }

    #[test]
    fn equals_form() {
        let a = cont(&["app", "--hwdec=vaapi", "--remote-debug-port=9222"]);
        assert_eq!(a.hwdec.as_deref(), Some("vaapi"));
        assert_eq!(a.remote_debugging_port, Some(9222));
    }

    #[test]
    fn bool_flags() {
        let a = cont(&["app", "--audio-exclusive", "--disable-gpu-compositing"]);
        assert!(a.audio_exclusive);
        assert!(a.disable_gpu_compositing);
    }

    #[test]
    fn platform_requires_x11() {
        let args = ["app", "--platform", "x11"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        assert!(matches!(parse(args, false), CliOutcome::Error(_)));
    }

    #[test]
    fn no_args_continue_all_unset() {
        let a = cont(&["app"]);
        assert!(a.hwdec.is_none());
        assert!(a.audio_passthrough.is_none());
        assert!(a.audio_channels.is_none());
        assert!(a.log_level.is_none());
        assert!(a.log_file.is_none());
        assert!(a.config_dir.is_none());
        assert!(a.cache_dir.is_none());
        assert!(a.ozone_platform.is_none());
        assert!(a.platform_override.is_none());
        assert!(!a.audio_exclusive);
        assert!(!a.disable_gpu_compositing);
        assert!(a.remote_debugging_port.is_none());
        assert!(a.x11_paint.is_none());
        assert!(a.wayland_paint.is_none());
    }

    #[test]
    fn x11_paint_values() {
        assert_eq!(
            cont(&["app", "--x11-paint=gpu"]).x11_paint,
            Some(X11Paint::Gpu)
        );
        assert_eq!(
            cont(&["app", "--x11-paint", "shm"]).x11_paint,
            Some(X11Paint::Shm)
        );
    }

    #[test]
    fn wayland_paint_values() {
        assert_eq!(
            cont(&["app", "--wayland-paint=dmabuf"]).wayland_paint,
            Some(WaylandPaint::Dmabuf)
        );
        assert_eq!(
            cont(&["app", "--wayland-paint", "shm"]).wayland_paint,
            Some(WaylandPaint::Shm)
        );
    }

    #[test]
    fn paint_unknown_value_errors() {
        match parse_args(&["app", "--x11-paint=bogus"]) {
            CliOutcome::Error(_) => {}
            _ => panic!("expected Error"),
        }
        match parse_args(&["app", "--wayland-paint=bogus"]) {
            CliOutcome::Error(_) => {}
            _ => panic!("expected Error"),
        }
    }

    #[test]
    fn paint_last_wins() {
        let a = cont(&["app", "--x11-paint=gpu", "--x11-paint", "shm"]);
        assert_eq!(a.x11_paint, Some(X11Paint::Shm));
        let a = cont(&["app", "--wayland-paint=dmabuf", "--wayland-paint", "shm"]);
        assert_eq!(a.wayland_paint, Some(WaylandPaint::Shm));
    }

    #[test]
    fn positional_is_error() {
        assert!(matches!(
            parse_args(&["app", "positional"]),
            CliOutcome::Error(_)
        ));
    }

    #[test]
    fn missing_trailing_value_is_error() {
        match parse_args(&["app", "--log-level"]) {
            CliOutcome::Error(s) => assert_eq!(s, "--log-level"),
            _ => panic!("expected Error"),
        }
    }

    #[test]
    fn space_form_all_flags() {
        let a = cont(&[
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
            "--platform",
            "x11",
        ]);
        assert_eq!(a.hwdec.as_deref(), Some("vaapi"));
        assert_eq!(a.log_level.as_deref(), Some("debug"));
        assert_eq!(a.log_file.as_deref(), Some("/tmp/x.log"));
        assert_eq!(a.config_dir.as_deref(), Some("/tmp/config"));
        assert_eq!(a.cache_dir.as_deref(), Some("/tmp/cache"));
        assert_eq!(a.audio_passthrough.as_deref(), Some("ac3,dts-hd"));
        assert_eq!(a.audio_channels.as_deref(), Some("5.1"));
        assert_eq!(a.remote_debugging_port, Some(9222));
        assert_eq!(a.ozone_platform.as_deref(), Some("wayland"));
        assert_eq!(a.platform_override.as_deref(), Some("x11"));
    }

    #[test]
    fn log_file_unset_vs_explicit_empty() {
        assert!(cont(&["app"]).log_file.is_none());
        assert_eq!(cont(&["app", "--log-file="]).log_file.as_deref(), Some(""));
    }

    #[test]
    fn remote_debug_port_non_numeric_error() {
        assert!(matches!(
            parse_args(&["app", "--remote-debug-port=bogus"]),
            CliOutcome::Error(_)
        ));
    }

    #[test]
    fn prefix_collision_log_level_vs_log_file() {
        let a = cont(&["app", "--log-file=path", "--log-level=trace"]);
        assert_eq!(a.log_file.as_deref(), Some("path"));
        assert_eq!(a.log_level.as_deref(), Some("trace"));
    }

    #[test]
    fn duplicate_flag_last_wins() {
        let a = cont(&["app", "--log-level=info", "--log-level", "debug"]);
        assert_eq!(a.log_level.as_deref(), Some("debug"));
    }

    #[test]
    fn error_leaves_no_partial_values() {
        // On error the outcome carries no CliArgs at all — partial values
        // can't leak by construction.
        assert!(matches!(
            parse_args(&[
                "app",
                "--hwdec",
                "vaapi",
                "--garbage",
                "--log-level",
                "debug"
            ]),
            CliOutcome::Error(_)
        ));
    }
}
