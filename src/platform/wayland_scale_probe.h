#pragma once

namespace wayland_scale_probe {

// Open an own wl_display connection, query xdg-output for fractional scale,
// disconnect. Returns the live fractional scale of the output that contains
// (x, y), or of the first output if x/y are negative. Returns 0 on failure
// (no Wayland session, no xdg-output, etc.) — caller should fall back to 1.0.
//
// Must be called BEFORE mpv_initialize so the result can scale-correct the
// --geometry option pre-init.
double query_scale(int x, int y);

}
