#pragma once

#include "include/cef_render_handler.h"
#include "include/internal/cef_types.h"
#include <functional>
#include <string>
#include <mpv/client.h>

enum class IdleInhibitLevel { None, System, Display };

struct Platform {
    void (*early_init)();
    bool (*init)(mpv_handle* mpv);
    void (*cleanup)();

    // Main browser subsurface
    void (*present)(const CefAcceleratedPaintInfo& info);
    void (*present_software)(const CefRenderHandler::RectList& dirty,
                             const void* buffer, int w, int h);
    void (*resize)(int lw, int lh, int pw, int ph);

    // Overlay browser subsurface
    void (*overlay_present)(const CefAcceleratedPaintInfo& info);
    void (*overlay_present_software)(const CefRenderHandler::RectList& dirty,
                                     const void* buffer, int w, int h);
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

    // Query the window's top-left screen position in logical pixels.
    // Returns false if unavailable. Used to save/restore window position.
    bool (*query_window_position)(int* x, int* y);

    // Clamp saved window geometry so it fits within the primary screen's
    // visible area. Called before mpv init so the window never appears
    // oversized or off-screen. Values are in the same coordinate system
    // as the --geometry option (backing pixels for size+position on macOS).
    // Implementations may be null (no clamping).
    void (*clamp_window_geometry)(int* w, int* h, int* x, int* y);

    void (*pump)();

    // macOS only — null elsewhere.
    // Block on the NSApplication run loop ([NSApp run]) until wake_main_loop
    // is called from initiate_shutdown. Drives NSEvents, GCD main-queue
    // blocks (mpv VO DispatchQueue.main.sync), the CEF wake source, and
    // CFRunLoopTimers — all event-driven, no polling.
    void (*run_main_loop)();
    // Stop the NSApplication run loop. Thread-safe; called from
    // initiate_shutdown to break out of run_main_loop.
    void (*wake_main_loop)();

    // Cursor shape/visibility (CT_NONE hides, others show with shape)
    void (*set_cursor)(cef_cursor_type_t type);

    // Idle inhibit: None = release, System = prevent sleep, Display = prevent sleep + display off
    void (*set_idle_inhibit)(IdleInhibitLevel level);

    // Titlebar color (KDE/KWin only, no-op on other compositors)
    void (*set_titlebar_color)(uint8_t r, uint8_t g, uint8_t b);

    // Whether the GPU can produce shared textures (dmabufs). Set during init.
    // When false, CEF should use software rendering (OnPaint) instead of
    // OnAcceleratedPaint, and present_software / overlay_present_software
    // must be non-null.
    bool shared_texture_supported = true;

    // CEF ozone platform. Resolved once in main() from use_wayland / --ozone-platform.
    // The dmabuf probe tests GL on this display.
    std::string cef_ozone_platform;

    // Read the system clipboard as UTF-8 text. Used by the context menu's
    // Paste action — CEF's frame->Paste() can't see external Wayland
    // selections under our forced --ozone-platform=x11 config, so we read
    // directly from the OS and inject via document.execCommand('insertText').
    //
    // Asynchronous: the callback fires when the text is available (or with
    // an empty string if the clipboard has no compatible text). On Wayland
    // this is driven by the input thread's existing poll loop — the pipe
    // fd from wl_data_offer_receive becomes a regular poll source. On
    // macOS/Windows the OS API is synchronous and the callback runs inline
    // on the calling thread. Callbacks may run on any thread.
    void (*clipboard_read_text_async)(std::function<void(std::string)> on_done);
};

#ifdef _WIN32
Platform make_windows_platform();
#elif defined(__APPLE__)
Platform make_macos_platform();
#elif defined(__linux__)
Platform make_wayland_platform();
#ifdef HAVE_X11
Platform make_x11_platform();
#endif
#endif
