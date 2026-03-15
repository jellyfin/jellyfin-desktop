#pragma once

#include <SDL3/SDL.h>
#include <cstdint>

// KDE/KWin per-window titlebar color via the server-decoration-palette
// Wayland protocol. Dynamically writes a .colors scheme file and tells
// KWin to apply it to our surface.
//
// TODO: X11 equivalent — set _KDE_NET_WM_COLOR_SCHEME window property
//       pointing at the same .colors file.

void initKdeDecorationPalette(SDL_Window* window);
void setKdeTitlebarColor(uint8_t r, uint8_t g, uint8_t b);
void cleanupKdeDecorationPalette();
