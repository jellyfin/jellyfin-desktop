#pragma once

#include "include/cef_render_handler.h"
#include "include/internal/cef_types.h"
#include <functional>
#include <mpv/client.h>

enum class IdleInhibitLevel { None, System, Display };

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
    // Delay, then fade overlay from opaque to transparent, then hide.
    // on_fade_start is called after the delay, just before the fade begins.
    // on_complete is called after the fade finishes.  Both may fire on any thread.
    void (*fade_overlay)(float delay_sec, float fade_sec,
                         std::function<void()> on_fade_start,
                         std::function<void()> on_complete);

    // Fullscreen
    void (*set_fullscreen)(bool fullscreen);
    void (*toggle_fullscreen)();

    // Fullscreen transitions (main surface only)
    void (*begin_transition)();
    void (*end_transition)();
    bool (*in_transition)();
    void (*set_expected_size)(int w, int h);

    float (*get_scale)();

    // Query logical content dimensions from the window system.
    // Returns false if unavailable (caller should use mpv osd-dimensions / scale).
    bool (*query_logical_content_size)(int* w, int* h);

    void (*pump)();

    // Cursor shape/visibility (CT_NONE hides, others show with shape)
    void (*set_cursor)(cef_cursor_type_t type);

    // Idle inhibit: None = release, System = prevent sleep, Display = prevent sleep + display off
    void (*set_idle_inhibit)(IdleInhibitLevel level);

    // Titlebar color (KDE/KWin only, no-op on other compositors)
    void (*set_titlebar_color)(uint8_t r, uint8_t g, uint8_t b);
};

#ifdef _WIN32
Platform make_windows_platform();
#elif defined(__APPLE__)
Platform make_macos_platform();
#elif defined(__linux__)
Platform make_wayland_platform();
#endif
