#include "platform_ops.h"
#include "platform.h"

#include <memory>
#include <string>

extern Platform g_platform;

namespace {

// Holds a (fn, ctx, dtor) Rust closure for the lifetime of the std::function
// it was wrapped in. dtor() runs from ~RustVoid when the last std::function
// copy is destroyed, letting the Rust side free its boxed state.
struct RustVoid {
    void (*fn)(void*) = nullptr;
    void* ctx = nullptr;
    void (*dtor)(void*) = nullptr;

    RustVoid(void (*f)(void*), void* c, void (*d)(void*)) : fn(f), ctx(c), dtor(d) {}
    RustVoid(const RustVoid&) = delete;
    RustVoid& operator=(const RustVoid&) = delete;
    ~RustVoid() { if (dtor) dtor(ctx); }
};

struct RustInt {
    void (*fn)(void*, int) = nullptr;
    void* ctx = nullptr;
    void (*dtor)(void*) = nullptr;

    RustInt(void (*f)(void*, int), void* c, void (*d)(void*)) : fn(f), ctx(c), dtor(d) {}
    RustInt(const RustInt&) = delete;
    RustInt& operator=(const RustInt&) = delete;
    ~RustInt() { if (dtor) dtor(ctx); }
};

struct RustString {
    void (*fn)(void*, const char*, size_t) = nullptr;
    void* ctx = nullptr;
    void (*dtor)(void*) = nullptr;

    RustString(void (*f)(void*, const char*, size_t), void* c, void (*d)(void*))
        : fn(f), ctx(c), dtor(d) {}
    RustString(const RustString&) = delete;
    RustString& operator=(const RustString&) = delete;
    ~RustString() { if (dtor) dtor(ctx); }
};

bool surface_present(void* s, const void* info) {
    if (!s || !info || !g_platform.surface_present) return false;
    return g_platform.surface_present(
        static_cast<PlatformSurface*>(s),
        *static_cast<const CefAcceleratedPaintInfo*>(info));
}

bool surface_present_software(void* s, const JfnRect* dirty, size_t n,
                              const void* buffer, int w, int h) {
    if (!s || !g_platform.surface_present_software) return false;
    CefRenderHandler::RectList rects;
    rects.reserve(n);
    for (size_t i = 0; i < n; i++)
        rects.emplace_back(dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h);
    return g_platform.surface_present_software(
        static_cast<PlatformSurface*>(s), rects, buffer, w, h);
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
    auto start = std::make_shared<RustVoid>(on_start, sctx, sdtor);
    auto done = std::make_shared<RustVoid>(on_done, dctx, ddtor);
    if (!s || !g_platform.fade_surface) {
        if (start->fn) start->fn(start->ctx);
        if (done->fn) done->fn(done->ctx);
        return;
    }
    g_platform.fade_surface(
        static_cast<PlatformSurface*>(s), sec,
        [start]() { if (start->fn) start->fn(start->ctx); },
        [done]() { if (done->fn) done->fn(done->ctx); });
}

void popup_show(void* s, const JfnPopupRequest* req) {
    if (!s || !g_platform.popup_show || !req) return;
    Platform::PopupRequest out;
    out.x = req->x;
    out.y = req->y;
    out.lw = req->lw;
    out.lh = req->lh;
    out.initial_highlight = req->initial_highlight;
    out.options.reserve(req->options_len);
    for (size_t i = 0; i < req->options_len; i++) {
        const char* opt = req->options ? req->options[i] : nullptr;
        out.options.emplace_back(opt ? opt : "");
    }
    if (req->on_selected) {
        auto holder = std::make_shared<RustInt>(
            req->on_selected, req->on_selected_ctx, req->on_selected_dtor);
        out.on_selected = [holder](int idx) {
            if (holder->fn) holder->fn(holder->ctx, idx);
        };
    }
    g_platform.popup_show(static_cast<PlatformSurface*>(s), out);
}

void popup_hide(void* s) {
    if (!s || !g_platform.popup_hide) return;
    g_platform.popup_hide(static_cast<PlatformSurface*>(s));
}

void popup_present(void* s, const void* info, int lw, int lh) {
    if (!s || !info || !g_platform.popup_present) return;
    g_platform.popup_present(static_cast<PlatformSurface*>(s),
                             *static_cast<const CefAcceleratedPaintInfo*>(info),
                             lw, lh);
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
    auto holder = std::make_shared<RustString>(cb, ctx, dtor);
    if (!g_platform.clipboard_read_text_async) {
        if (holder->fn) holder->fn(holder->ctx, "", 0);
        return;
    }
    g_platform.clipboard_read_text_async([holder](std::string text) {
        if (holder->fn) holder->fn(holder->ctx, text.data(), text.size());
    });
}

void open_external_url(const char* utf8, size_t len) {
    if (!g_platform.open_external_url || !utf8) return;
    g_platform.open_external_url(std::string(utf8, len));
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
