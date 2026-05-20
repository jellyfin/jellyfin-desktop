#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// True once a wp_fractional_scale_v1.preferred_scale event has been seen.
// Until then logical dims computed from physical dims would be wrong on a
// fractional display, so main.cpp waits on this before computing initial
// CefLayer dimensions.
bool jfn_wl_scale_known(void);

// Current fractional scale (1.0 until a preferred_scale event arrives).
float jfn_wl_get_cached_scale(void);

// Register Rust-owned callbacks against the wl-proxy:
//   - wp_fractional_scale_v1.preferred_scale → updates cached scale
//   - xdg_toplevel.configure → forwards into the runtime resize path
//     and posts synthetic OSD-dim pixels via jfn_playback_post_osd_pixels.
// Must run before mpv_create so the very first compositor configure +
// preferred_scale events are captured — otherwise main.cpp computes
// initial dims with scale=1.0 and CEF overshoots.
void jfn_wl_register_proxy_callbacks(void);

#ifdef __cplusplus
}
#endif
