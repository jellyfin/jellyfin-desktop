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

#ifdef __cplusplus
}
#endif
