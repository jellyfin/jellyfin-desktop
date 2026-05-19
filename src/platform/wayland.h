#pragma once

namespace platform::wayland {

// Hand the jfn-wayland::proxy Rust crate our xdg_toplevel.configure callback
// and register the preferred-scale callback that owns the cached fractional
// scale. Called from main.cpp before mpv_create so the very first compositor
// configure + preferred_scale events are captured. Otherwise main.cpp would
// compute initial logical dims with scale = 1.0 and CEF overshoots.
//
// Subsequent configure events drive the runtime resize path (on_mpv_configure
// inside wayland.cpp) and post OSD pixel dimensions to the playback
// coordinator via jfn_playback_post_osd_pixels.
//
// Safe to call before wl_init has run — the callback's downstream helpers
// guard against empty g_wl state and a null playback coordinator.
void register_proxy_callbacks();

}  // namespace platform::wayland
