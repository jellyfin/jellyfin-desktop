#include "window_geometry.h"
#include "settings.h"
#include "logging.h"
#include <cstring>

static bool isWayland() {
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver && strcmp(driver, "wayland") == 0;
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

    // Restore maximized state (do this AFTER position/size so maximize
    // happens on the correct screen with correct pre-maximize geometry)
    if (geom.maximized) {
        SDL_MaximizeWindow(window);
        LOG_INFO(LOG_WINDOW, "Restored maximized state");
    }
}

void saveWindowGeometry(SDL_Window* window, bool pre_fullscreen_maximized) {
    Settings::WindowGeometry geom;

    SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    geom.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    bool fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;

    // If fullscreen, size/position reflect the monitor, not the user's
    // windowed geometry. Save only the pre-fullscreen maximized state.
    if (fullscreen) {
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
