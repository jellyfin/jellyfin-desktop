#include "window_geometry.h"
#include "settings.h"
#include "logging.h"
#include <cstring>

static bool isWayland() {
    static const bool result = [] {
        const char* driver = SDL_GetCurrentVideoDriver();
        return driver && strcmp(driver, "wayland") == 0;
    }();
    return result;
}

void restoreWindowGeometry(SDL_Window* window) {
    const auto& geom = Settings::instance().windowGeometry();

    // Restore size if saved (non-zero)
    if (geom.width > 0 && geom.height > 0) {
        SDL_SetWindowSize(window, geom.width, geom.height);
        LOG_INFO(LOG_WINDOW, "Restored window size: %dx%d", geom.width, geom.height);
    }

    // Restore position if saved and not on Wayland
    if (!isWayland() && geom.x >= 0 && geom.y >= 0) {
        SDL_SetWindowPosition(window, geom.x, geom.y);
        LOG_INFO(LOG_WINDOW, "Restored window position: %d,%d", geom.x, geom.y);
    }

    // Clamp to display bounds before maximizing
    clampWindowToDisplay(window);

    // Restore maximized state (do this AFTER position/size so maximize
    // happens on the correct screen with correct pre-maximize geometry)
    if (geom.maximized) {
        SDL_MaximizeWindow(window);
        LOG_INFO(LOG_WINDOW, "Restored maximized state");
    }
}

void clampWindowToDisplay(SDL_Window* window) {
    SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    if (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))
        return;

    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (!display) return;

    SDL_Rect bounds;
    if (!SDL_GetDisplayUsableBounds(display, &bounds)) return;

    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    int clamped_w = (w > bounds.w) ? bounds.w : w;
    int clamped_h = (h > bounds.h) ? bounds.h : h;

    if (clamped_w != w || clamped_h != h) {
        SDL_SetWindowSize(window, clamped_w, clamped_h);
        LOG_INFO(LOG_WINDOW, "Clamped window size from %dx%d to %dx%d (display: %dx%d)",
                 w, h, clamped_w, clamped_h, bounds.w, bounds.h);
    }
}

void saveWindowGeometry(SDL_Window* window, bool pre_fullscreen_maximized) {
    Settings::WindowGeometry geom;

    SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    geom.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    bool fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;

    // If fullscreen, size/position reflect the monitor, not the user's
    // windowed geometry. Preserve previously-saved windowed geometry and
    // only update the maximized flag.
    if (fullscreen) {
        geom = Settings::instance().windowGeometry();
        geom.maximized = pre_fullscreen_maximized;
        Settings::instance().setWindowGeometry(geom);
        Settings::instance().save();
        LOG_INFO(LOG_WINDOW, "Saved geometry (fullscreen): maximized=%d",
                 pre_fullscreen_maximized);
        return;
    }

    if (geom.maximized) {
        // When maximized, size/position reflect the maximized geometry,
        // not the user's windowed geometry. Only save the maximized flag.
        // On next launch, SDL_MaximizeWindow will handle sizing.
        geom.width = 0;
        geom.height = 0;
        geom.x = -1;
        geom.y = -1;
    } else {
        SDL_GetWindowSize(window, &geom.width, &geom.height);
        if (!isWayland()) {
            SDL_GetWindowPosition(window, &geom.x, &geom.y);
        }
    }

    Settings::instance().setWindowGeometry(geom);
    Settings::instance().save();

    LOG_INFO(LOG_WINDOW, "Saved window geometry: %dx%d at %d,%d maximized=%d",
             geom.width, geom.height, geom.x, geom.y, geom.maximized);
}
