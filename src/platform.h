#pragma once

#include "include/cef_render_handler.h"
#include <mpv/client.h>

class Client;
class OverlayClient;

struct Platform {
    void (*early_init)();
    bool (*init)(mpv_handle* mpv);
    void (*cleanup)();

    // Main browser subsurface
    void (*present)(const CefAcceleratedPaintInfo& info);
    void (*present_software)(const void* buffer, int w, int h);
    void (*resize)(int lw, int lh, int pw, int ph);

    // Overlay browser subsurface
    void (*overlay_present)(const CefAcceleratedPaintInfo& info);
    void (*overlay_present_software)(const void* buffer, int w, int h);
    void (*overlay_resize)(int lw, int lh, int pw, int ph);
    void (*set_overlay_visible)(bool visible);
    void (*fade_overlay)(float duration_sec);  // fade overlay from opaque to transparent, then hide

    // Fullscreen
    void (*set_fullscreen)(bool fullscreen);
    void (*toggle_fullscreen)();

    // Fullscreen transitions (main surface only)
    void (*begin_transition)();
    void (*end_transition)();
    bool (*in_transition)();
    void (*set_expected_size)(int w, int h);

    float (*get_scale)();
    void (*pump)();

    // Titlebar color (KDE/KWin only, no-op on other compositors)
    void (*set_titlebar_color)(uint8_t r, uint8_t g, uint8_t b);
};

#ifdef __linux__
Platform make_wayland_platform();
#endif
