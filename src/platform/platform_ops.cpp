#include "platform_ops.h"
#include "platform.h"
#include "../common.h"

#include <cstddef>

// Owning definitions for the two C++ globals; main.cpp shrinks during the
// Rust port and can no longer host them. Declarations live in
// platform/platform.h (g_platform) and common.h (g_video_bg).
Platform g_platform{};
Color g_video_bg{};

// The Wayland backend authors `make_wayland_platform()` in Rust
// (src/wayland/src/make_platform.rs) and returns the Platform vtable by
// value. The Rust side hand-mirrors this struct with `#[repr(C)]`; any
// drift in field order, types, or alignment would silently misdispatch
// vtable calls. Pin the layout here so any future edit to `struct
// Platform` triggers a compile error if the Rust mirror would no longer
// agree.
static_assert(sizeof(Platform) == 320,
              "Platform size changed — update Rust mirror in "
              "src/wayland/src/make_platform.rs");
static_assert(offsetof(Platform, display) == 0);
static_assert(offsetof(Platform, early_init) == 8);
static_assert(offsetof(Platform, init) == 16);
static_assert(offsetof(Platform, cleanup) == 24);
static_assert(offsetof(Platform, post_window_cleanup) == 32);
static_assert(offsetof(Platform, alloc_surface) == 40);
static_assert(offsetof(Platform, free_surface) == 48);
static_assert(offsetof(Platform, surface_present) == 56);
static_assert(offsetof(Platform, surface_present_software) == 64);
static_assert(offsetof(Platform, surface_resize) == 72);
static_assert(offsetof(Platform, surface_set_visible) == 80);
static_assert(offsetof(Platform, restack) == 88);
static_assert(offsetof(Platform, fade_surface) == 96);
static_assert(offsetof(Platform, popup_show) == 104);
static_assert(offsetof(Platform, popup_hide) == 112);
static_assert(offsetof(Platform, popup_present) == 120);
static_assert(offsetof(Platform, popup_present_software) == 128);
static_assert(offsetof(Platform, set_fullscreen) == 136);
static_assert(offsetof(Platform, toggle_fullscreen) == 144);
static_assert(offsetof(Platform, begin_transition) == 152);
static_assert(offsetof(Platform, end_transition) == 160);
static_assert(offsetof(Platform, in_transition) == 168);
static_assert(offsetof(Platform, set_expected_size) == 176);
static_assert(offsetof(Platform, get_scale) == 184);
static_assert(offsetof(Platform, get_display_scale) == 192);
static_assert(offsetof(Platform, query_window_position) == 200);
static_assert(offsetof(Platform, clamp_window_geometry) == 208);
static_assert(offsetof(Platform, pump) == 216);
static_assert(offsetof(Platform, run_main_loop) == 224);
static_assert(offsetof(Platform, wake_main_loop) == 232);
static_assert(offsetof(Platform, set_cursor) == 240);
static_assert(offsetof(Platform, set_idle_inhibit) == 248);
static_assert(offsetof(Platform, set_theme_color) == 256);
static_assert(offsetof(Platform, shared_texture_supported) == 264);
static_assert(offsetof(Platform, cef_ozone_platform) == 265);
static_assert(offsetof(Platform, clipboard_read_text_async) == 304);
static_assert(offsetof(Platform, open_external_url) == 312);

namespace {

bool surface_present(void* s, const void* info) {
    if (!s || !info || !g_platform.surface_present) return false;
    return g_platform.surface_present(static_cast<PlatformSurface*>(s), info);
}

bool surface_present_software(void* s, const JfnRect* dirty, size_t n,
                              const void* buffer, int w, int h) {
    if (!s || !g_platform.surface_present_software) return false;
    return g_platform.surface_present_software(
        static_cast<PlatformSurface*>(s), dirty, n, buffer, w, h);
}

void surface_resize(void* s, int lw, int lh, int pw, int ph) {
    if (!s || !g_platform.surface_resize) return;
    g_platform.surface_resize(static_cast<PlatformSurface*>(s), lw, lh, pw, ph);
}

void surface_set_visible(void* s, bool v) {
    if (!s || !g_platform.surface_set_visible) return;
    g_platform.surface_set_visible(static_cast<PlatformSurface*>(s), v);
}

void fade_surface(void* s, float sec,
                  void (*on_start)(void*), void* sctx, void (*sdtor)(void*),
                  void (*on_done)(void*), void* dctx, void (*ddtor)(void*)) {
    if (!s || !g_platform.fade_surface) {
        if (on_start) on_start(sctx);
        if (sdtor) sdtor(sctx);
        if (on_done) on_done(dctx);
        if (ddtor) ddtor(dctx);
        return;
    }
    g_platform.fade_surface(static_cast<PlatformSurface*>(s), sec,
                            on_start, sctx, sdtor,
                            on_done, dctx, ddtor);
}

void popup_show(void* s, const JfnPopupRequest* req) {
    if (!req) return;
    if (!s || !g_platform.popup_show) {
        if (req->on_selected_dtor) req->on_selected_dtor(req->on_selected_ctx);
        return;
    }
    g_platform.popup_show(static_cast<PlatformSurface*>(s), req);
}

void popup_hide(void* s) {
    if (!s || !g_platform.popup_hide) return;
    g_platform.popup_hide(static_cast<PlatformSurface*>(s));
}

void popup_present(void* s, const void* info, int lw, int lh) {
    if (!s || !info || !g_platform.popup_present) return;
    g_platform.popup_present(static_cast<PlatformSurface*>(s), info, lw, lh);
}

void popup_present_software(void* s, const void* buffer, int pw, int ph,
                            int lw, int lh) {
    if (!s || !g_platform.popup_present_software) return;
    g_platform.popup_present_software(static_cast<PlatformSurface*>(s),
                                      buffer, pw, ph, lw, lh);
}

void set_fullscreen(bool v) {
    if (g_platform.set_fullscreen) g_platform.set_fullscreen(v);
}

void set_cursor(int type) {
    if (g_platform.set_cursor)
        g_platform.set_cursor(static_cast<cef_cursor_type_t>(type));
}

void clipboard_read_text_async(
    void (*cb)(void*, const char*, size_t), void* ctx, void (*dtor)(void*)) {
    if (!g_platform.clipboard_read_text_async) {
        if (cb) cb(ctx, "", 0);
        if (dtor) dtor(ctx);
        return;
    }
    g_platform.clipboard_read_text_async(cb, ctx, dtor);
}

void open_external_url(const char* utf8, size_t len) {
    if (!g_platform.open_external_url || !utf8) return;
    g_platform.open_external_url(utf8, len);
}

constexpr JfnPlatformOps kOps = {
    surface_present,
    surface_present_software,
    surface_resize,
    surface_set_visible,
    fade_surface,
    popup_show,
    popup_hide,
    popup_present,
    popup_present_software,
    set_fullscreen,
    set_cursor,
    clipboard_read_text_async,
    open_external_url,
};

}  // namespace

extern "C" const JfnPlatformOps* jfn_platform_ops(void) {
    return &kOps;
}

// Field accessors for jfn_wayland::lifecycle. The wl_init port needs to
// read cef_ozone_platform (for the dmabuf probe) and write
// shared_texture_supported / clipboard_read_text_async during init.

extern "C" const char* jfn_platform_cef_ozone_platform(void) {
    return g_platform.cef_ozone_platform;
}

extern "C" void jfn_platform_set_shared_texture_unsupported(void) {
    g_platform.shared_texture_supported = false;
}

extern "C" void jfn_platform_clear_clipboard_handler(void) {
    g_platform.clipboard_read_text_async = nullptr;
}

// Toggle fullscreen state. Used by Rust app_menu's "Toggle Fullscreen"
// dispatch and the playerOsdActive cleanup path.
extern "C" void jfn_platform_toggle_fullscreen(void) {
    if (g_platform.toggle_fullscreen) g_platform.toggle_fullscreen();
}

extern "C" void jfn_platform_set_fullscreen(bool v) {
    if (g_platform.set_fullscreen) g_platform.set_fullscreen(v);
}

extern "C" void jfn_platform_set_cursor(int type_) {
    if (g_platform.set_cursor)
        g_platform.set_cursor(static_cast<cef_cursor_type_t>(type_));
}

extern "C" void* jfn_platform_alloc_surface(void) {
    return g_platform.alloc_surface ? g_platform.alloc_surface() : nullptr;
}

extern "C" void jfn_platform_free_surface(void* s) {
    if (s && g_platform.free_surface)
        g_platform.free_surface(static_cast<PlatformSurface*>(s));
}

extern "C" void jfn_platform_restack(void* const* ordered, size_t n) {
    if (g_platform.restack)
        g_platform.restack(
            reinterpret_cast<PlatformSurface* const*>(ordered), n);
}

// Raw accessor for the Rust jfn_app_main port: lets Rust populate the
// global Platform vtable on Linux (where make_*_platform lives in Rust).
// The pointer is stable for the lifetime of the process.
extern "C" Platform* jfn_g_platform_ptr(void) {
    return &g_platform;
}

// Field/method accessors used by jfn_app_main (jfn_rust/src/app.rs). The
// Rust port reads the Platform vtable through these instead of mirroring
// the whole struct shape on the Rust side.

extern "C" int jfn_g_platform_display(void) {
    return static_cast<int>(g_platform.display);
}

extern "C" float jfn_g_platform_get_scale(void) {
    return g_platform.get_scale ? g_platform.get_scale() : 1.0f;
}

extern "C" float jfn_g_platform_get_display_scale(int x, int y) {
    return g_platform.get_display_scale ? g_platform.get_display_scale(x, y) : 1.0f;
}

extern "C" void jfn_g_platform_clamp_window_geometry(int* w, int* h, int* x, int* y) {
    if (g_platform.clamp_window_geometry)
        g_platform.clamp_window_geometry(w, h, x, y);
}

extern "C" void jfn_g_platform_pump(void) {
    if (g_platform.pump) g_platform.pump();
}

extern "C" bool jfn_g_platform_query_window_position(int* x, int* y) {
    return g_platform.query_window_position
        ? g_platform.query_window_position(x, y) : false;
}

extern "C" void jfn_g_platform_post_window_cleanup(void) {
    if (g_platform.post_window_cleanup) g_platform.post_window_cleanup();
}

extern "C" void jfn_g_video_bg_set(uint32_t rgb) {
    g_video_bg = Color{rgb};
}
