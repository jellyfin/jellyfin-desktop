#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open a short-lived Wayland registry pass on the supplied (mpv-owned)
// `wl_display`, bind `org_kde_kwin_server_decoration_palette_manager`, and
// create the per-window palette object bound to `parent_surface`. Also
// seeds the colors directory under $XDG_RUNTIME_DIR/jellyfin-desktop.
// Returns false if the compositor isn't KWin (protocol not advertised),
// XDG_RUNTIME_DIR is unset, or any wire step fails. Safe to call once
// during wl_init.
bool jfn_wl_kde_palette_attach(void* display, void* parent_surface);

// Write a KDE color-scheme file for the given color and dispatch
// `set_palette(path)` on the bound palette object. No-op (no wire traffic)
// when the requested color matches the previously written one. Safe to
// call from any thread. `hex` must be exactly 7 bytes of the form
// "#RRGGBB" (the `Color::hex` field).
void jfn_wl_kde_palette_set_color(uint8_t r, uint8_t g, uint8_t b,
                                  const char* hex);

// Remove the currently-active scheme file. Called after the window has
// been torn down so KWin's last read of the file completes first. The
// palette object itself is dropped atomically by KWin with the window.
void jfn_wl_kde_palette_post_window_cleanup(void);

#ifdef __cplusplus
}
#endif
