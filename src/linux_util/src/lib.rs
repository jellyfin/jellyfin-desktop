//! Linux-only platform helpers shared by the X11 and Wayland backends:
//! systemd-logind idle inhibition and external-URL launching. Merged from
//! the former `jfn-idle-inhibit-linux` / `jfn-open-url-linux` crates so the
//! two single-function helpers live in one place.
//!
//! The whole crate is `#![cfg(target_os = "linux")]`, so it's an empty rlib
//! elsewhere and the workspace builds uniformly on every platform.

#![cfg(target_os = "linux")]

pub mod idle_inhibit;
pub mod open_url;

use jfn_platform_abi::WindowDecorations;

/// KDE draws its own server-side decorations and lets us tint them via the
/// palette protocol; elsewhere (notably GNOME) nothing draws them, so we draw
/// our own client-side titlebar.
pub fn default_window_decorations() -> WindowDecorations {
    let kde = std::env::var("XDG_CURRENT_DESKTOP")
        .map(|v| v.split(':').any(|s| s.eq_ignore_ascii_case("KDE")))
        .unwrap_or(false);
    if kde {
        WindowDecorations::ServerThemed
    } else {
        WindowDecorations::Csd
    }
}
