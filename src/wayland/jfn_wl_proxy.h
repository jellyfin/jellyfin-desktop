#pragma once

#include <stdbool.h>
#include "../wlproxy/wlproxy.h"   // JfnConfigureCb

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

// Register the wp_fractional_scale_v1.preferred_scale callback (Rust-owned;
// it updates the cached scale read by jfn_wl_get_cached_scale) and forwards
// the supplied xdg_toplevel.configure callback to the wl-proxy. Must run
// before mpv_create so the very first compositor configure + preferred_scale
// events are captured — otherwise main.cpp computes initial dims with
// scale=1.0 and CEF overshoots.
void jfn_wl_register_proxy_callbacks(JfnConfigureCb configure_cb);

#ifdef __cplusplus
}
#endif
