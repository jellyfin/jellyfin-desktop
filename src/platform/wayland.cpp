#include "common.h"
#include "platform/platform.h"
#include "jfn_wayland_scale_probe.h"
#include "clipboard/wayland.h"
#include "jfn_idle_inhibit_linux.h"
#include "jfn_open_url_linux.h"
#include "input/input_wayland.h"
#include "playback/jfn_ingest.h"
#include "jfn_fade.h"
#include "jfn_wl_core.h"
#include "jfn_wl_proxy.h"
#include "jfn_kde_palette.h"

#include <unistd.h>

// =====================================================================
// All Wayland state + surface ops + present/transition machinery now
// lives in src/wayland (jfn-wayland crate). This file is a thin
// trampoline layer: it builds the Platform vtable, unpacks CEF-typed
// structs (CefAcceleratedPaintInfo, PopupRequest) into plain C structs,
// and routes calls into jfn_wl_* FFI.
// =====================================================================

namespace {

// Translate a CEF accelerated paint into a JfnDmabufFrame. CEF owns the
// source fd — the trampoline dup()'s it so libwayland-client can close
// the wire copy after marshalling without affecting CEF's lifecycle.
bool to_dmabuf_frame(const CefAcceleratedPaintInfo& info, JfnDmabufFrame& out) {
    int fd = dup(info.planes[0].fd);
    if (fd < 0) return false;
    out.fd        = fd;
    out.stride    = info.planes[0].stride;
    out.modifier  = info.modifier;
    out.coded_w   = info.extra.coded_size.width;
    out.coded_h   = info.extra.coded_size.height;
    out.visible_w = info.extra.visible_rect.width;
    out.visible_h = info.extra.visible_rect.height;
    return true;
}

// ---- Vtable trampolines ----------------------------------------------

PlatformSurface* wl_alloc_surface() {
    return static_cast<PlatformSurface*>(jfn_wl_alloc_surface());
}

void wl_free_surface(PlatformSurface* s) {
    jfn_wl_free_surface(s);
}

void wl_restack(PlatformSurface* const* ordered, size_t n) {
    jfn_wl_restack(reinterpret_cast<void* const*>(ordered), n);
}

bool wl_surface_present(PlatformSurface* s, const CefAcceleratedPaintInfo& info) {
    JfnDmabufFrame f{};
    if (!to_dmabuf_frame(info, f)) return false;
    return jfn_wl_surface_present(s, &f);
}

bool wl_surface_present_software(PlatformSurface* s,
                                 const CefRenderHandler::RectList&,
                                 const void* buffer, int w, int h) {
    return jfn_wl_surface_present_software(
        s, static_cast<const uint8_t*>(buffer), w, h);
}

void wl_surface_resize(PlatformSurface* s, int lw, int lh, int pw, int ph) {
    jfn_wl_surface_resize(s, lw, lh, pw, ph);
}

void wl_surface_set_visible(PlatformSurface* s, bool visible) {
    jfn_wl_surface_set_visible(s, visible, kBgColor.r, kBgColor.g, kBgColor.b);
}

void wl_popup_show(PlatformSurface* s, const Platform::PopupRequest& req) {
    jfn_wl_popup_show(s, req.x, req.y, req.lw, req.lh);
}

void wl_popup_hide(PlatformSurface* s) {
    jfn_wl_popup_hide(s);
}

void wl_popup_present(PlatformSurface* s, const CefAcceleratedPaintInfo& info,
                      int lw, int lh) {
    JfnDmabufFrame f{};
    if (!to_dmabuf_frame(info, f)) return;
    jfn_wl_popup_present(s, &f, lw, lh);
}

void wl_popup_present_software(PlatformSurface* s, const void* buffer,
                               int pw, int ph, int lw, int lh) {
    jfn_wl_popup_present_software(
        s, static_cast<const uint8_t*>(buffer), pw, ph, lw, lh);
}

void wl_set_fullscreen(bool fullscreen) {
    jfn_wl_set_fullscreen(fullscreen);
}

void wl_toggle_fullscreen() {
    jfn_wl_toggle_fullscreen();
}

void wl_begin_transition() {
    jfn_wl_begin_transition();
}

void wl_end_transition() {
    jfn_wl_end_transition();
}

bool wl_in_transition() {
    return jfn_wl_in_transition();
}

void wl_set_expected_size(int, int) {}

void wl_pump() {}

void wl_set_idle_inhibit(IdleInhibitLevel level) {
    jfn_idle_inhibit_set(static_cast<uint32_t>(level));
}

float wl_get_scale() {
    return jfn_wl_get_cached_scale();
}

float wl_get_display_scale(int x, int y) {
    double s = jfn_wayland_scale_probe(x, y);
    return s > 0.0 ? static_cast<float>(s) : 1.0f;
}

// ---- KDE titlebar color shims --------------------------------------

void wl_post_window_cleanup() {
    jfn_wl_kde_palette_post_window_cleanup();
}

void wl_set_theme_color(const Color& c) {
    jfn_wl_kde_palette_set_color(c.r, c.g, c.b, c.hex);
}

// ---- Lifecycle -------------------------------------------------------

bool wl_init(mpv_handle* /*mpv*/) { return jfn_wl_lifecycle_init(); }
void wl_cleanup() { jfn_wl_lifecycle_cleanup(); }

// ---- Fade trampoline (keeps std::function wrapping in C++) -----------

void invoke_fn(void* ctx) {
    auto* f = static_cast<std::function<void()>*>(ctx);
    if (*f) (*f)();
}
void delete_fn(void* ctx) {
    delete static_cast<std::function<void()>*>(ctx);
}

void wl_fade_surface(PlatformSurface* s, float fade_sec,
                     std::function<void()> on_fade_start,
                     std::function<void()> on_complete) {
    double fps = jfn_playback_display_hz();
    if (!s || fps <= 0) {
        if (on_fade_start) on_fade_start();
        if (on_complete) on_complete();
        return;
    }
    auto* start_ctx = new std::function<void()>(std::move(on_fade_start));
    auto* done_ctx  = new std::function<void()>(std::move(on_complete));
    jfn_wl_fade_start(s, fade_sec, fps, jfn_wl_fade_apply_frame,
                      invoke_fn, start_ctx, delete_fn,
                      invoke_fn, done_ctx,  delete_fn);
}

} // namespace

// =====================================================================
// Platform vtable
// =====================================================================

Platform make_wayland_platform() {
    return Platform{
        .display = DisplayBackend::Wayland,
        .early_init = []() {},
        .init = wl_init,
        .cleanup = wl_cleanup,
        .post_window_cleanup = wl_post_window_cleanup,
        .alloc_surface = wl_alloc_surface,
        .free_surface = wl_free_surface,
        .surface_present = wl_surface_present,
        .surface_present_software = wl_surface_present_software,
        .surface_resize = wl_surface_resize,
        .surface_set_visible = wl_surface_set_visible,
        .restack = wl_restack,
        .fade_surface = wl_fade_surface,
        .popup_show = wl_popup_show,
        .popup_hide = wl_popup_hide,
        .popup_present = wl_popup_present,
        .popup_present_software = wl_popup_present_software,
        .set_fullscreen = wl_set_fullscreen,
        .toggle_fullscreen = wl_toggle_fullscreen,
        .begin_transition = wl_begin_transition,
        .end_transition = wl_end_transition,
        .in_transition = wl_in_transition,
        .set_expected_size = wl_set_expected_size,
        .get_scale = wl_get_scale,
        .get_display_scale = wl_get_display_scale,
        .query_window_position = [](int*, int*) -> bool { return false; },
        .clamp_window_geometry = nullptr,
        .pump = wl_pump,
        .set_cursor = input::wayland::set_cursor,
        .set_idle_inhibit = wl_set_idle_inhibit,
        .set_theme_color = wl_set_theme_color,
        .clipboard_read_text_async = clipboard_wayland::read_text_async,
        .open_external_url = [](const std::string& url) { jfn_open_url(url.c_str()); },
    };
}
