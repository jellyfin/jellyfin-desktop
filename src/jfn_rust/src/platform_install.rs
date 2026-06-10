//! Composition root for the `Platform` backend: the only place that
//! decides which backend a build installs, and when.
//!
//! Windows and macOS have exactly one backend, known at compile time;
//! Linux picks Wayland or X11 at runtime. That difference also dictates
//! ordering — see the two install functions.

#[cfg(target_os = "linux")]
use jfn_platform_abi::{DisplayBackend, Platform};

/// Install the backend on OSes with a single compile-time backend
/// (Windows, macOS). Must run before `jfn_cef_start`: CEF subprocesses
/// bail out of the browser-process flow but may still query the
/// platform. No-op on Linux ([`install_from_cli`] runs there instead).
pub fn install_early() {
    #[cfg(target_os = "windows")]
    {
        let p = jfn_windows::make_windows_platform();
        p.early_init();
        jfn_platform_abi::install(p);
    }
    #[cfg(target_os = "macos")]
    {
        let p = jfn_macos::make_macos_platform();
        p.early_init();
        jfn_platform_abi::install(p);
    }
}

/// Install the backend on Linux, where Wayland vs X11 is chosen at
/// runtime from `--platform` / session env. Runs after logging init so
/// the backend choice is logged. No-op on OSes whose backend
/// [`install_early`] already installed.
pub fn install_from_cli(cli: &crate::cli::Cli) {
    #[cfg(target_os = "linux")]
    {
        let backend = match cli.platform {
            Some(crate::cli::PlatformArg::Wayland) => DisplayBackend::Wayland,
            Some(crate::cli::PlatformArg::X11) => DisplayBackend::X11,
            None => {
                let has_wayland = std::env::var_os("WAYLAND_DISPLAY").is_some();
                let has_display = std::env::var_os("DISPLAY").is_some();
                if has_wayland || !has_display {
                    DisplayBackend::Wayland
                } else {
                    DisplayBackend::X11
                }
            }
        };
        if let Some(p) = cli.platform_paint {
            match backend {
                DisplayBackend::Wayland => jfn_wayland::set_paint_override(match p {
                    crate::cli::Paint::Dmabuf => jfn_wayland::WlPaintOverride::Dmabuf,
                    crate::cli::Paint::Gpu => jfn_wayland::WlPaintOverride::Gpu,
                    crate::cli::Paint::Shm => jfn_wayland::WlPaintOverride::Shm,
                }),
                DisplayBackend::X11 => jfn_x11::set_paint_override(match p {
                    crate::cli::Paint::Dmabuf => jfn_x11::X11PaintOverride::Dmabuf,
                    crate::cli::Paint::Gpu => jfn_x11::X11PaintOverride::Gpu,
                    crate::cli::Paint::Shm => jfn_x11::X11PaintOverride::Shm,
                }),
                _ => {}
            }
        }

        let p: Box<dyn Platform> = match backend {
            DisplayBackend::Wayland => jfn_wayland::make_platform::make_wayland_platform(),
            DisplayBackend::X11 => jfn_x11::make_platform::make_x11_platform(),
            _ => unreachable!(),
        };
        p.early_init();
        jfn_platform_abi::install(p);
        tracing::info!(target: "Main", "Display backend: {}",
            if backend == DisplayBackend::Wayland { "wayland" } else { "x11" });
    }
    #[cfg(not(target_os = "linux"))]
    let _ = cli;
}
