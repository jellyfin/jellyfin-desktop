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
