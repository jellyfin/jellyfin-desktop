#pragma once

#include "include/cef_render_handler.h"
#include "include/internal/cef_types.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <mpv/client.h>

enum class IdleInhibitLevel { None, System, Display };

#include "display_backend.h"
#include "platform_ops.h"
#include "../color.h"

#include <cstddef>

// Opaque per-backend surface handle. Each backend defines the layout in
// its translation unit; callers only ever hold pointers.
struct PlatformSurface;

struct Platform {
    DisplayBackend display;

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
    // present/present_software return true when the buffer was attached to
    // the surface. A false return means the platform dropped this paint
    // (e.g. Wayland tolerance gate during a resize transition); callers
    // that track "renderer stabilised" state must not count dropped paints.
    //
    // accel_paint_info is a `const CefAcceleratedPaintInfo*` opaque to
    // this header; backends that consume it cast internally. Keeping the
    // vtable free of CEF C++ types lets the struct cross as repr(C).
    bool (*surface_present)(PlatformSurface*, const void* accel_paint_info);
    bool (*surface_present_software)(PlatformSurface*,
                                     const JfnRect* dirty, size_t dirty_len,
                                     const void* buffer, int w, int h);
    void (*surface_resize)(PlatformSurface*, int lw, int lh, int pw, int ph);

    void (*surface_set_visible)(PlatformSurface*, bool visible);

    // Stacking — bottom (index 0) to top (index n-1). Called whenever
    // the Browsers vector order changes.
    void (*restack)(PlatformSurface* const* ordered, size_t n);

    // Window-resize signal — outbound from the platform. Fires when the
    // Optional per-surface fade — finite UI animation; backends limited
    // to a single fadeable surface return /no-op for the others.
    // Each (fn, ctx, dtor) triple: fn(ctx) is invoked once at the
    // corresponding moment; dtor(ctx) runs once when the backend drops
    // its last reference (allowing the caller to free boxed state).
    // Either fn or dtor may be null.
    void (*fade_surface)(PlatformSurface*, float fade_sec,
                         void (*on_fade_start)(void*), void* start_ctx,
                         void (*start_dtor)(void*),
                         void (*on_complete)(void*), void* done_ctx,
                         void (*done_dtor)(void*));

    // Popup (CEF OSR popup elements, e.g. <select> dropdowns).
    //
    // CefLayer calls popup_show once per popup with everything any backend
    // might need; the backend picks what it uses. Compositor backends
    // (Wayland, Windows) use rect + popup_present[_software] frames and
    // ignore options/initial_highlight/on_selected — CEF dispatches
    // selection internally on click. Native-menu backends (macOS / NSMenu)
    // use options + initial_highlight + on_selected and ignore the
    // present frames.
    //
    // on_selected may fire on any thread. Request layout matches
    // JfnPopupRequest in platform_ops.h so the platform_ops thunk can
    // forward the pointer with no rebuild.
    void (*popup_show)(PlatformSurface*, const JfnPopupRequest* req);
    void (*popup_hide)(PlatformSurface*);
    void (*popup_present)(PlatformSurface*, const void* accel_paint_info, int lw, int lh);
    void (*popup_present_software)(PlatformSurface*, const void* buffer, int pw, int ph, int lw, int lh);

    // Fullscreen
    void (*set_fullscreen)(bool fullscreen);
    void (*toggle_fullscreen)();

    // Fullscreen transitions (main surface only)
    void (*begin_transition)();
    void (*end_transition)();
    bool (*in_transition)();
    void (*set_expected_size)(int w, int h);

    float (*get_scale)();

    // Live display scale at screen point (x, y), queried from the OS.
    // Returns a positive float; backends return 1.0f on query failure.
    float (*get_display_scale)(int x, int y);

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
    // mpv CAMetalLayer fills; X11/Windows are no-ops. Backends recompute
    // r/g/b/hex from the packed rgb word internally; the vtable stays free
    // of C++ types so it can cross as repr(C).
    void (*set_theme_color)(uint32_t rgb);

    // Whether the GPU can produce shared textures (dmabufs). Set during init.
    // When false, CEF should use software rendering (OnPaint) instead of
    // OnAcceleratedPaint, and present_software / overlay_present_software
    // must be non-null. Each factory sets this explicitly (typically true).
    bool shared_texture_supported;

    // CEF ozone platform. Resolved once in main() from display backend / --ozone-platform.
    // The dmabuf probe tests GL on this display. NUL-terminated, fixed-size
    // so the Platform struct stays POD/repr(C)-friendly. Factories
    // zero-initialize; main() overwrites.
    char cef_ozone_platform[32];

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
    // utf8 pointer is valid only for the duration of the callback; it
    // may be empty (len == 0) if the clipboard has no compatible text.
    // dtor(ctx) runs once after the callback fires (or immediately if
    // no callback path was taken). Either fn or dtor may be null.
    void (*clipboard_read_text_async)(void (*on_done)(void* ctx,
                                                       const char* utf8,
                                                       size_t len),
                                       void* ctx,
                                       void (*dtor)(void* ctx));

    // Caller guarantees non-empty UTF-8 URL not starting with '-'. `utf8`
    // is NUL-terminated for `len` bytes.
    void (*open_external_url)(const char* utf8, size_t len);
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

// Internal platform factories — called by make_platform(). All four
// are authored in Rust now and exported with C linkage:
//   src/wayland/src/make_platform.rs
//   src/x11/src/make_platform.rs
//   src/macos/src/lib.rs
//   src/windows/src/lib.rs
extern "C" Platform make_wayland_platform();
#ifdef HAVE_X11
extern "C" Platform make_x11_platform();
#endif
extern "C" Platform make_macos_platform();
extern "C" Platform make_windows_platform();

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
    // Hand the Rust-side CefLayer port (src/jfn_cef/src/client.rs) a vtable
    // of thunks that wrap g_platform. Safe to register before g_platform is
    // assigned: each thunk reads g_platform at call time, not at registration.
    jfn_cef_set_platform_ops(jfn_platform_ops());
    return p;
}
