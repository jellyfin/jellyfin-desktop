#include "window_activation.h"
#include "logging.h"

#if !defined(__APPLE__) && !defined(_WIN32)
#include <cstring>
#include <wayland-client.h>
#include "wayland-protocols/xdg-activation-v1-client.h"
#endif

namespace {

#if !defined(__APPLE__) && !defined(_WIN32)
struct xdg_activation_v1* g_xdg_activation = nullptr;
struct wl_surface* g_wl_surface = nullptr;
#endif

} // namespace

void initWindowActivation(SDL_Window* window) {
#if !defined(__APPLE__) && !defined(_WIN32)
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    auto* display = static_cast<struct wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    g_wl_surface = static_cast<struct wl_surface*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));

    if (!display || !g_wl_surface)
        return;

    auto registry_handler = [](void* data, wl_registry* registry,
                               uint32_t name, const char* interface, uint32_t version) {
        if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
            *static_cast<xdg_activation_v1**>(data) = static_cast<xdg_activation_v1*>(
                wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
        }
    };
    static const wl_registry_listener listener = {
        .global = +registry_handler,
        .global_remove = [](void*, wl_registry*, uint32_t) {},
    };
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, &g_xdg_activation);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);

    if (g_xdg_activation)
        LOG_INFO(LOG_MAIN, "Bound xdg_activation_v1 for window activation");
#else
    (void)window;
#endif
}

void activateWindow(SDL_Window* window, const std::string& token) {
#if !defined(__APPLE__) && !defined(_WIN32)
    if (g_xdg_activation && g_wl_surface && !token.empty()) {
        xdg_activation_v1_activate(g_xdg_activation, token.c_str(), g_wl_surface);
        LOG_INFO(LOG_MAIN, "Activated window via xdg-activation-v1 token");
    }
#endif
    SDL_RestoreWindow(window);
    SDL_RaiseWindow(window);
}

void cleanupWindowActivation() {
#if !defined(__APPLE__) && !defined(_WIN32)
    if (g_xdg_activation) {
        xdg_activation_v1_destroy(g_xdg_activation);
        g_xdg_activation = nullptr;
    }
    g_wl_surface = nullptr;
#endif
}
