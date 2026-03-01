#pragma once

#include <SDL3/SDL.h>
#include <string>

// Bind platform-specific window activation protocol (e.g. xdg-activation-v1 on Wayland).
// Call once after window creation, before the event loop.
void initWindowActivation(SDL_Window* window);

// Raise and focus the window, using platform activation if available.
// token carries the activation token from the signaling instance (empty if unavailable).
void activateWindow(SDL_Window* window, const std::string& token);

// Release protocol resources. Call during shutdown.
void cleanupWindowActivation();
