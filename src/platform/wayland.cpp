#include "common.h"
#include "platform/platform.h"
#include "jfn_wayland_scale_probe.h"
#include "jfn_wl_proxy.h"
#include "clipboard/wayland.h"
#include "jfn_idle_inhibit_linux.h"
#include "jfn_open_url_linux.h"
#include "input/input_wayland.h"
#include "mpv/jfn_mpv_api.h"
#include "playback/jfn_ingest.h"
#include "jfn_dmabuf_probe.h"
#include "jfn_fade.h"
#include "jfn_wl_core.h"

#include <wayland-client.h>
#include <EGL/egl.h>
#ifdef HAVE_KDE_DECORATION_PALETTE
#include "jfn_kde_palette.h"
#endif
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include "logging.h"

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

// ---- KDE titlebar color shim (Wayland protocol piece kept here because
// org_kde_kwin_server_decoration_palette has no Rust binding) -----------

#ifdef HAVE_KDE_DECORATION_PALETTE
#include "server-decoration-palette-client.h"

static org_kde_kwin_server_decoration_palette_manager* g_palette_manager = nullptr;
static org_kde_kwin_server_decoration_palette*         g_palette         = nullptr;
static wl_display*                                     g_palette_display = nullptr;
static wl_surface*                                     g_palette_parent  = nullptr;

// Registry listener used only by the KDE palette path. The Rust
// wl_state registry binds standard globals; we keep this tiny extra
// registry pass for the palette manager only.
static void palette_reg_global(void* /*data*/, wl_registry* reg,
                               uint32_t name, const char* iface, uint32_t /*ver*/) {
    if (strcmp(iface, org_kde_kwin_server_decoration_palette_manager_interface.name) == 0) {
        g_palette_manager = static_cast<org_kde_kwin_server_decoration_palette_manager*>(
            wl_registry_bind(reg, name,
                             &org_kde_kwin_server_decoration_palette_manager_interface, 1));
    }
}
static void palette_reg_remove(void*, wl_registry*, uint32_t) {}
static const wl_registry_listener s_palette_reg = {
    .global = palette_reg_global, .global_remove = palette_reg_remove
};

static void wl_init_kde_palette() {
    if (!g_palette_display || !g_palette_parent) return;
    wl_event_queue* q = wl_display_create_queue(g_palette_display);
    auto* reg = wl_display_get_registry(g_palette_display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(reg), q);
    wl_registry_add_listener(reg, &s_palette_reg, nullptr);
    wl_display_roundtrip_queue(g_palette_display, q);
    wl_registry_destroy(reg);
    wl_event_queue_destroy(q);

    if (!g_palette_manager) return;
    g_palette = org_kde_kwin_server_decoration_palette_manager_create(
        g_palette_manager, g_palette_parent);
    if (!g_palette) return;
    if (!jfn_wl_kde_palette_init()) {
        org_kde_kwin_server_decoration_palette_release(g_palette);
        g_palette = nullptr;
        return;
    }
    LOG_INFO(LOG_PLATFORM, "KDE decoration palette ready");
}

static void wl_cleanup_kde_palette() {
    // Don't release the palette object — KWin atomically drops it with
    // the window. Releasing now would flash the system color back.
    g_palette = nullptr;
    g_palette_manager = nullptr;
}

static void wl_post_window_cleanup() {
    jfn_wl_kde_palette_post_window_cleanup();
}

static void wl_set_theme_color(const Color& c) {
    if (!g_palette) return;
    const char* path = jfn_wl_kde_palette_write(c.r, c.g, c.b, c.hex);
    if (!path) return;
    org_kde_kwin_server_decoration_palette_set_palette(g_palette, path);
    wl_display_flush(g_palette_display);
}
#else
static void wl_init_kde_palette() {}
static void wl_cleanup_kde_palette() {}
static void wl_post_window_cleanup() {}
static void wl_set_theme_color(const Color&) {}
#endif

// ---- on_proxy_configure ----------------------------------------------

// Fires from the wl-proxy per-client thread for every xdg_toplevel.configure
// from the compositor. Authoritative size source on Wayland.
extern "C" void on_proxy_configure(int physical_w, int physical_h, int fullscreen) {
    if (physical_w <= 0 || physical_h <= 0) return;
    jfn_wl_on_configure(physical_w, physical_h, fullscreen);
    float scale = jfn_wl_get_cached_scale();
    if (scale <= 0.f) scale = 1.0f;
    jfn_playback_post_osd_pixels(physical_w, physical_h, scale, false, 0, 0);
}

// ---- Lifecycle -------------------------------------------------------

bool wl_init(mpv_handle* /*mpv*/) {
    intptr_t dp = 0, sp = 0;
    {
        int64_t v = 0;
        if (jfn_mpv_get_property_int("wayland-display", &v) == 0) dp = static_cast<intptr_t>(v);
        v = 0;
        if (jfn_mpv_get_property_int("wayland-surface", &v) == 0) sp = static_cast<intptr_t>(v);
    }
    if (!dp || !sp) {
        LOG_ERROR(LOG_PLATFORM, "Failed to get Wayland display/surface from mpv");
        return false;
    }
    auto* display = reinterpret_cast<wl_display*>(dp);
    auto* parent  = reinterpret_cast<wl_surface*>(sp);

    // Seed Rust state with mpv's current fullscreen — first configure
    // after this point won't start a spurious transition.
    jfn_wl_core_set_was_fullscreen(jfn_playback_fullscreen());

    // Prepare the input layer first so its xkb context is ready before
    // any seat_caps wires up keyboard listeners that need xkb.
    input::wayland::init(display);

    if (!jfn_wl_core_init(display, parent)) {
        LOG_ERROR(LOG_PLATFORM, "jfn_wl_core_init failed");
        return false;
    }

#ifdef HAVE_KDE_DECORATION_PALETTE
    g_palette_display = display;
    g_palette_parent  = parent;
#endif

    // Register close callback — intercepts xdg_toplevel close before mpv sees it.
    {
        intptr_t cb_ptr = 0;
        int64_t v = 0;
        if (jfn_mpv_get_property_int("wayland-close-cb-ptr", &v) == 0)
            cb_ptr = static_cast<intptr_t>(v);
        if (cb_ptr) {
            auto* fn   = reinterpret_cast<void(**)(void*)>(cb_ptr);
            auto* data = reinterpret_cast<void**>(cb_ptr + sizeof(void*));
            *fn   = [](void*) { initiate_shutdown(); };
            *data = nullptr;
        }
    }

    // EGL init for CEF shared texture support + dmabuf probe.
    EGLDisplay egl_dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display));
    if (egl_dpy != EGL_NO_DISPLAY) eglInitialize(egl_dpy, nullptr, nullptr);

    if (!jfn_wl_dmabuf_probe(g_platform.cef_ozone_platform.c_str(), egl_dpy)) {
        LOG_WARN(LOG_PLATFORM, "Shared textures not supported; using software CEF rendering");
        g_platform.shared_texture_supported = false;
    }

    wl_init_kde_palette();

    input::wayland::start_input_thread();

    clipboard_wayland::init();
    if (!clipboard_wayland::available())
        g_platform.clipboard_read_text_async = nullptr;

    return true;
}

void wl_cleanup() {
    jfn_wl_fade_stop_all();

    // Null the close trampoline before tearing down state it would read.
    {
        intptr_t cb_ptr = 0;
        int64_t v = 0;
        if (jfn_mpv_get_property_int("wayland-close-cb-ptr", &v) == 0)
            cb_ptr = static_cast<intptr_t>(v);
        if (cb_ptr) {
            auto* fn = reinterpret_cast<void(**)(void*)>(cb_ptr);
            *fn = nullptr;
        }
    }

    wl_cleanup_kde_palette();
    jfn_idle_inhibit_cleanup();
    clipboard_wayland::cleanup();
    input::wayland::cleanup();
    // Rust-side WlState lives until process exit (mirrors C++ globals).
}

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
// Public registration shim (called from main.cpp before mpv_create so
// the very first compositor configure/preferred_scale events are captured).
// =====================================================================

namespace platform::wayland {
void register_proxy_callbacks() {
    jfn_wl_register_proxy_callbacks(on_proxy_configure);
}
}

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
