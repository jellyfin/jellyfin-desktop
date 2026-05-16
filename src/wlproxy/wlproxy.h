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
// compositor to mpv. Args: width, height, fullscreen (1/0). The event is
// forwarded to mpv after the callback runs. Fires from the proxy's per-client
// thread — callback must be thread-safe.
typedef void (*JfnConfigureCb)(int width, int height, int fullscreen);
void jfn_wlproxy_set_configure_callback(JfnConfigureCb cb);

#ifdef __cplusplus
}
#endif
