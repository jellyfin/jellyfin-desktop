// Umbrella staticlib. Each member crate is an rlib so the workspace shares a
// single copy of std/core in the final binary. MSVC link.exe rejects duplicate
// symbols across staticlibs, so we cannot ship one staticlib per member.
//
// `pub use ... ::*` forces rustc to monomorphize each rlib's public surface
// into this crate, which keeps every `#[unsafe(no_mangle)] pub extern "C"` C
// entry point visible in the resulting `libjfn_rust` archive.

pub use jfn_cli::*;
pub use jfn_config::*;
pub use jfn_jellyfin::*;
pub use jfn_log_redact::*;
pub use jfn_paths::*;
pub use jfn_single_instance::*;
pub use jfn_wake_event::*;

#[cfg(unix)]
pub use jfn_signal_guard::*;

#[cfg(target_os = "linux")]
pub use jfn_clipboard_wayland::*;

#[cfg(target_os = "linux")]
pub use jfn_idle_inhibit_linux::*;

#[cfg(target_os = "linux")]
pub use jfn_open_url_linux::*;

#[cfg(target_os = "linux")]
pub use jfn_wayland_scale_probe::*;

#[cfg(target_os = "linux")]
pub use jfn_wlproxy::*;
