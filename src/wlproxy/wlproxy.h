#pragma once

// Pure-forwarder Wayland proxy. Vet-only — no interception.
// mpv connects to this proxy (via WAYLAND_DISPLAY env) instead of the real
// compositor. All messages forward untouched in both directions.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Proxy JfnWlproxy;

JfnWlproxy* jfn_wlproxy_start(void);
const char* jfn_wlproxy_display_name(const JfnWlproxy* p);
void jfn_wlproxy_stop(JfnWlproxy* p);

// Register a callback fired on each xdg_toplevel.configure event from
// compositor to mpv. Args: width, height (physical pixels, already scaled by
// fractional_scale), fullscreen (1/0). The event still forwards to mpv after
// the callback runs. Fires from the proxy's per-client thread — callback
// must be thread-safe.
typedef void (*JfnConfigureCb)(int width, int height, int fullscreen);
void jfn_wlproxy_set_configure_callback(JfnConfigureCb cb);

// Register a callback fired on each wp_fractional_scale_v1.preferred_scale
// event. scale_120 is the numerator over WAYLAND_SCALE_FACTOR=120 (120 = 1.0x,
// 180 = 1.5x, 240 = 2.0x). Fires from the proxy's per-client thread.
typedef void (*JfnScaleCb)(int scale_120);
void jfn_wlproxy_set_scale_callback(JfnScaleCb cb);

// Queue an xdg_toplevel.set_fullscreen / unset_fullscreen request applied by
// the proxy on its next dispatch iteration (~16ms). Thread-safe.
void jfn_wlproxy_set_fullscreen(int enable);

// Queue an xdg_toplevel.set_maximized / unset_maximized request applied by
// the proxy on its next dispatch iteration (~16ms). Thread-safe.
void jfn_wlproxy_set_maximized(int enable);

#ifdef __cplusplus
}
#endif
