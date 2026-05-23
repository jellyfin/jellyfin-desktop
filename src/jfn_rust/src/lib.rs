// Umbrella staticlib. Each member crate is an rlib so the workspace shares a
// single copy of std/core in the final binary. MSVC link.exe rejects duplicate
// symbols across staticlibs, so we cannot ship one staticlib per member.
//
// `pub use ... ::*` forces rustc to monomorphize each rlib's public surface
// into this crate, which keeps every `#[unsafe(no_mangle)] pub extern "C"` C
// entry point visible in the resulting `libjfn_rust` archive.

pub use jfn_cef::*;
pub use jfn_cli::*;
pub use jfn_color::*;
pub use jfn_config::*;
pub use jfn_jellyfin::*;
pub use jfn_logging::*;
pub use jfn_mpv::boot::*;
pub use jfn_mpv::probe::*;
pub use jfn_paths::*;
pub use jfn_playback::*;
pub use jfn_single_instance::*;

#[cfg(unix)]
pub use jfn_signal_guard::*;

#[cfg(target_os = "linux")]
pub use jfn_idle_inhibit_linux::*;

#[cfg(target_os = "linux")]
pub use jfn_open_url_linux::*;

#[cfg(target_os = "linux")]
pub use jfn_wayland::clipboard::*;

#[cfg(target_os = "linux")]
pub use jfn_wayland::dmabuf_probe::*;

#[cfg(target_os = "linux")]
pub use jfn_wayland::fade::*;

#[cfg(target_os = "linux")]
pub use jfn_wayland::input::*;

#[cfg(all(target_os = "linux", feature = "kde-palette"))]
pub use jfn_wayland::kde_palette::*;

#[cfg(target_os = "linux")]
pub use jfn_wayland::proxy::*;

#[cfg(target_os = "linux")]
pub use jfn_wayland::scale_probe::*;

#[cfg(target_os = "linux")]
pub use jfn_wlproxy::*;

#[cfg(target_os = "linux")]
pub use jfn_x11::make_platform::make_x11_platform;

#[cfg(target_os = "linux")]
pub use jfn_x11::surface::{
    jfn_x11_alloc_surface, jfn_x11_fade_surface, jfn_x11_free_surface, jfn_x11_restack,
    jfn_x11_surface_present, jfn_x11_surface_present_software, jfn_x11_surface_resize,
    jfn_x11_surface_set_visible,
};
