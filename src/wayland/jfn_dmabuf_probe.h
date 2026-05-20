#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Test whether GBM -> EGL image -> GL texture binding works on the EGL
// display CEF will use. Run once during wl_init; if false, the platform
// falls back to software CEF rendering.
//
// `ozone_platform` is `g_platform.cef_ozone_platform` (NUL-terminated).
// `wayland_egl_dpy` is the already-initialised EGLDisplay for the active
// wl_display (used when ozone_platform == "wayland"); may be NULL otherwise.
//
// Returns true when the probe succeeds or when libgbm / a DRM render node
// is unavailable (assume-supported fallback matches the original C++).
bool jfn_wl_dmabuf_probe(const char* ozone_platform, void* wayland_egl_dpy);

#ifdef __cplusplus
}
#endif
