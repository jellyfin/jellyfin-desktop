#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the colors directory under $XDG_RUNTIME_DIR/jellyfin-desktop.
// Returns false if XDG_RUNTIME_DIR is unset or the directory cannot be
// created. The Wayland protocol bindings (palette_manager / palette proxy)
// remain on the C++ side; this only manages the on-disk scheme files.
bool jfn_wl_kde_palette_init(void);

// Write a KDE color-scheme file for the given color and return its path
// (NUL-terminated UTF-8, valid until the next call). Returns NULL when the
// requested color matches the previously written one, when the palette is
// not initialised, or when the write fails.
//
// `hex` must be exactly 7 bytes of the form "#RRGGBB" (the `Color::hex`
// field).
const char* jfn_wl_kde_palette_write(uint8_t r, uint8_t g, uint8_t b,
                                     const char* hex);

// Remove the currently-active scheme file. Called after the window has been
// torn down so KWin's last read of the file completes first.
void jfn_wl_kde_palette_post_window_cleanup(void);

#ifdef __cplusplus
}
#endif
