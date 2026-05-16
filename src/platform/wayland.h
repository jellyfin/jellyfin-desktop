#pragma once

namespace platform::wayland {

// True when wp_fractional_scale_v1.preferred_scale has delivered at least
// one value. Until then we shouldn't compute logical dims from physical —
// the result will be wrong on a fractional display.
bool scale_known();

// Registers the proxy's xdg_toplevel.configure interception callback.
// Called from main.cpp before mpv_initialize so the very first compositor
// configure is captured. The callback drives the runtime resize path
// (on_mpv_configure) and publishes OSD_DIMS-equivalent state to the
// playback coordinator via mpv::set_osd_dims — replacing mpv's
// osd-dimensions observation on the Wayland backend.
//
// Safe to call before wl_init has run — the callback's downstream helpers
// guard against empty g_wl state and a null playback coordinator.
void register_proxy_callbacks();

}  // namespace platform::wayland
