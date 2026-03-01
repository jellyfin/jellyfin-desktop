#pragma once

#include <SDL3/SDL.h>

// Restores saved window geometry (size, position, maximized state).
// Call BEFORE showing the window or entering the event loop.
// On Wayland, position is ignored (compositor controls it).
void restoreWindowGeometry(SDL_Window* window);

// Saves current window geometry to settings.
// Call during shutdown, before SDL_DestroyWindow.
// If fullscreen, saves pre_fullscreen_maximized instead of current state.
void saveWindowGeometry(SDL_Window* window, bool pre_fullscreen_maximized = false);
