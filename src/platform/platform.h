#pragma once

#include "include/cef_render_handler.h"
#include "include/internal/cef_types.h"
#include <functional>
#include <string>
#include <vector>
#include <mpv/client.h>

enum class IdleInhibitLevel { None, System, Display };

#include "display_backend.h"
#include "../color.h"

#include <cstddef>

// Opaque per-backend surface handle. Each backend defines the layout in
// its translation unit; callers only ever hold pointers.
struct PlatformSurface;

struct Platform {
    DisplayBackend display{};

    void (*early_init)();
    bool (*init)(mpv_handle* mpv);
    void (*cleanup)();
    // Optional: runs after mpv has destroyed the window, for cleanup that
    // would otherwise be visible (e.g. removing a per-window kwin palette
    // file while the window is still on-screen). May be null.
    void (*post_window_cleanup)();

    // Layer-surface lifecycle. alloc_surface creates a generic surface
    // for the next CefLayer; free_surface tears it down.
    PlatformSurface* (*alloc_surface)();
    void (*free_surface)(PlatformSurface*);

    // Per-surface ops (replace the role-specific present/resize/visible triples).
    void (*surface_present)(PlatformSurface*, const CefAcceleratedPaintInfo& info);
    void (*surface_present_software)(PlatformSurface*,
                                     const CefRenderHandler::RectList& dirty,
                                     const void* buffer, int w, int h);
    void (*surface_resize)(PlatformSurface*, int lw, int lh, int pw, int ph);
    void (*surface_set_visible)(PlatformSurface*, bool visible);

    // Stacking — bottom (index 0) to top (index n-1). Called whenever
    // the Browsers vector order changes.
    void (*restack)(PlatformSurface* const* ordered, size_t n);

    // Window-wide resize broadcast. Fires when mpv reports a new OSD size
    // (Browsers::setSize is the broadcast point). Backends that maintain
    // window-scoped state (e.g. wayland's mpv_pw used by present-time size
    // guards, or the cef-main viewport against compositor bounds) hook this
    // to stay current. May be null.
    void (*window_resized)(int lw, int lh, int pw, int ph);

    // Optional per-surface fade — finite UI animation; backends limited
    // to a single fadeable surface return /no-op for the others.
    void (*fade_surface)(PlatformSurface*, float fade_sec,
                         std::function<void()> on_fade_start,
                         std::function<void()> on_complete);

    // Popup subsurface (CEF OSR popup elements, e.g. <select> dropdowns).
    // Parent surface is the currently-painting layer's surface, supplied
    // by the layer at popup time.
    void (*popup_show)(int x, int y, int lw, int lh);
    void (*popup_hide)();
    void (*popup_present)(const CefAcceleratedPaintInfo& info, int lw, int lh);
    void (*popup_present_software)(const void* buffer, int pw, int ph, int lw, int lh);

    // Attempt to show the platform's native popup menu for a <select>
    // dropdown. Returns true if the native menu will be shown (in which
    // case the compositor-based popup_show path should NOT be used).
    // Returns false on platforms without a native path; caller falls back
    // to the compositor popup subsurface.
    //
    // on_selected is invoked with the chosen option index, or -1 if the
    // user dismissed without selecting. Callers must assume it may fire
    // on any thread (e.g. macOS invokes it from the AppKit main thread).
    bool (*try_native_popup_menu)(int x, int y, int lw, int lh,
                                  const std::vector<std::string>& options,
                                  int current_index,
                                  std::function<void(int)> on_selected);

    // Fullscreen
    void (*set_fullscreen)(bool fullscreen);
    void (*toggle_fullscreen)();

    // Fullscreen transitions (main surface only)
    void (*begin_transition)();
    void (*end_transition)();
    bool (*in_transition)();
    void (*set_expected_size)(int w, int h);

    float (*get_scale)();

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

    // Chrome color: drives every native surface that should track the
    // current theme color so resize gaps and titlebar match. On Wayland/KDE
    // this writes a kwin palette file; on macOS it sets NSWindow + the
    // mpv CAMetalLayer fills; X11/Windows are no-ops.
    void (*set_theme_color)(const Color&);

    // Whether the GPU can produce shared textures (dmabufs). Set during init.
    // When false, CEF should use software rendering (OnPaint) instead of
    // OnAcceleratedPaint, and present_software / overlay_present_software
    // must be non-null.
    bool shared_texture_supported = true;

    // CEF ozone platform. Resolved once in main() from display backend / --ozone-platform.
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

    // Caller guarantees non-empty URL not starting with '-'.
    void (*open_external_url)(const std::string& url);
};

// Platform lifetime. Must destruct after CefRuntimeScope, before mpv teardown.
class PlatformScope {
public:
    PlatformScope(Platform& p, mpv_handle* mpv) : p_(p), ok_(p.init(mpv)) {}
    ~PlatformScope() { if (ok_) p_.cleanup(); }
    bool ok() const { return ok_; }

    PlatformScope(const PlatformScope&) = delete;
    PlatformScope& operator=(const PlatformScope&) = delete;
private:
    Platform& p_;
    bool ok_;
};

// Defined in main.cpp.
extern Platform g_platform;

// Releases the platform idle inhibit on any exit path.
struct IdleInhibitGuard {
    ~IdleInhibitGuard() { g_platform.set_idle_inhibit(IdleInhibitLevel::None); }
};

// Internal platform factories — called by make_platform()
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

inline Platform make_platform(DisplayBackend backend) {
    Platform p;
#ifdef _WIN32
    (void)backend;
    p = make_windows_platform();
#elif defined(__APPLE__)
    (void)backend;
    p = make_macos_platform();
#else
    switch (backend) {
    case DisplayBackend::Wayland: p = make_wayland_platform(); break;
#ifdef HAVE_X11
    case DisplayBackend::X11: p = make_x11_platform(); break;
#endif
    default: __builtin_unreachable();
    }
#endif
    p.early_init();
    return p;
}
