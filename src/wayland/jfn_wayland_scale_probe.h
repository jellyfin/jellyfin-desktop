#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Open an own wl_display connection, query xdg-output for fractional scale,
// disconnect. Returns the live fractional scale of the output containing
// (x, y), or of the first output if x/y are negative. Returns 0.0 on failure
// (no Wayland session, no xdg-output, etc.) — caller should fall back to 1.0.
//
// Must be called BEFORE mpv_initialize so the result can scale-correct the
// --geometry option pre-init.
double jfn_wayland_scale_probe(int x, int y);

#ifdef __cplusplus
}
#endif
