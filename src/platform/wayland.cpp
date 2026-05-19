#include "common.h"
#include "cef/cef_client.h"
#include "platform/platform.h"
#include "jfn_wayland_scale_probe.h"
#include "jfn_wl_proxy.h"
#include "clipboard/wayland.h"
#include "jfn_idle_inhibit_linux.h"
#include "jfn_open_url_linux.h"
#include "input/input_wayland.h"
#include "mpv/jfn_mpv_api.h"
#include "playback/jfn_ingest.h"
#include "wlproxy/wlproxy.h"
#include "jfn_dmabuf_probe.h"
#include "jfn_fade.h"

#include <wayland-client.h>
#include "linux-dmabuf-v1-client.h"
#include "viewporter-client.h"
#include "alpha-modifier-v1-client.h"
#include <drm/drm_fourcc.h>
#include <EGL/egl.h>
#ifdef HAVE_KDE_DECORATION_PALETTE
#include "server-decoration-palette-client.h"
#include "jfn_kde_palette.h"
#endif
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <mutex>
#include <vector>
#include <sys/mman.h>
#include "logging.h"


// =====================================================================
// Wayland state (file-static)
// =====================================================================

// Per-surface state. One per CefLayer (allocated by wl_alloc_surface,
// destroyed by wl_free_surface). Each surface owns its own popup
// subsurface so popups (e.g. <select> dropdowns) are children of the
// layer that spawned them — automatically z-ordered above the layer,
// no parent inference needed.
struct PlatformSurface {
    wl_surface*    surface = nullptr;
    wl_subsurface* subsurface = nullptr;
    wp_viewport*   viewport = nullptr;
    wp_alpha_modifier_surface_v1* alpha = nullptr;
    wl_buffer*     buffer = nullptr;
    int            buffer_w = 0, buffer_h = 0;  // physical pixels of `buffer`
    bool           visible = true;       // unmapped surfaces don't present
    bool           placeholder = false;  // true while showing solid-color placeholder
    bool           null_attached = false;// true while surface has wl_surface_attach(nullptr)
    // Per-surface logical/physical size — written by wl_surface_resize
    // (OSD_DIMS path) and on_mpv_configure (xdg_toplevel.configure fan-
    // out). Authoritative target for this surface's viewport math and
    // tolerance gate.
    int            lw = 0, lh = 0;
    int            pw = 0, ph = 0;

    // Per-surface popup (CEF OSR popup elements, e.g. <select> dropdowns).
    // The popup subsurface is a child of `surface`, so it draws above this
    // surface automatically.
    wl_surface*    popup_surface = nullptr;
    wl_subsurface* popup_subsurface = nullptr;
    wp_viewport*   popup_viewport = nullptr;
    wl_buffer*     popup_buffer = nullptr;
    bool           popup_visible = false;

};

struct WlState {
    std::mutex surface_mtx;  // protects surface ops between CEF thread and VO thread
    wl_display* display = nullptr;
    wl_event_queue* queue = nullptr;  // dedicated queue, isolated from mpv's
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_surface* parent = nullptr;

    // Current stack order, bottom-to-top. The first (bottom-most) surface
    // is treated as the cef-main surface for transition purposes.
    std::vector<PlatformSurface*> stack;  // guarded by surface_mtx

    // Shared globals
    wl_shm* shm = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_alpha_modifier_v1* alpha_modifier = nullptr;

    // cached_scale lives in jfn-wayland::proxy (Rust). C++ reads it via
    // jfn_wl_get_cached_scale() / jfn_wl_scale_known().
    bool was_fullscreen = false;
    // Resize transition state. transitioning gates non-paint paths
    // (resize, configure, fullscreen reject). Paint path uses a function-
    // pointer swap (g_present) — see present_drop / present_match_or_drop
    // / present_attach.
    bool transitioning = false;

#ifdef HAVE_KDE_DECORATION_PALETTE
    org_kde_kwin_server_decoration_palette_manager* palette_manager = nullptr;
    org_kde_kwin_server_decoration_palette* palette = nullptr;
#endif
};

static WlState g_wl;

// Fade thread + stop flag live in Rust (jfn_fade.h). wl_cleanup must call
// jfn_wl_fade_stop_all() before destroying the alpha modifier proxy or the
// surfaces — Alt+F4 mid-fade otherwise races destruction against the next
// iteration's set_multiplier/commit.

static void update_surface_size_locked(int lw, int lh, int pw, int ph);
static void wl_begin_transition_locked();
static void wl_end_transition_locked();
static void wl_begin_transition();
static void wl_toggle_fullscreen();
static void popup_create_locked(PlatformSurface* s);
static void popup_destroy_locked(PlatformSurface* s);
static void wl_init_kde_palette();
static void wl_cleanup_kde_palette();
static void wl_set_theme_color(const Color& c);

// Create a 1x1 ARGB8888 wl_buffer filled with a solid color.
// Uses an anonymous shm fd — the buffer is self-contained.
static wl_buffer* create_solid_color_buffer(const Color& c) {
    if (!g_wl.shm) return nullptr;
    const int stride = 4, size = stride;  // 1x1 pixel, 4 bytes
    int fd = memfd_create("solid-color", MFD_CLOEXEC);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, size) < 0) { close(fd); return nullptr; }
    auto* data = static_cast<uint8_t*>(mmap(nullptr, size, PROT_WRITE, MAP_SHARED, fd, 0));
    if (data == MAP_FAILED) { close(fd); return nullptr; }
    // ARGB8888: [B, G, R, A]
    data[0] = c.b; data[1] = c.g; data[2] = c.r; data[3] = 0xFF;
    munmap(data, size);
    auto* pool = wl_shm_create_pool(g_wl.shm, fd, size);
    auto* buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

// =====================================================================
// Generic per-surface present/resize/visibility (called by Browsers via
// the Platform vtable). The cef-main role lives on stack[0]: its present
// path participates in fullscreen transitions; other surfaces always
// pass through.
// =====================================================================

static wl_buffer* create_dmabuf_buffer(const CefAcceleratedPaintInfo& info) {
    int fd = dup(info.planes[0].fd);
    if (fd < 0) return nullptr;
    uint32_t stride = info.planes[0].stride;
    uint64_t modifier = info.modifier;
    int w = info.extra.coded_size.width;
    int h = info.extra.coded_size.height;

    auto* params = zwp_linux_dmabuf_v1_create_params(g_wl.dmabuf);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, modifier >> 32, modifier & 0xffffffff);
    auto* buf = zwp_linux_buffer_params_v1_create_immed(params, w, h, DRM_FORMAT_ARGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);
    return buf;
}

static wl_buffer* create_shm_buffer(const void* pixels, int w, int h) {
    if (!g_wl.shm) return nullptr;
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("cef-sw", MFD_CLOEXEC);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, size) < 0) { close(fd); return nullptr; }
    void* data = mmap(nullptr, size, PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return nullptr; }
    memcpy(data, pixels, size);
    munmap(data, size);
    auto* pool = wl_shm_create_pool(g_wl.shm, fd, size);
    auto* buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

// Common attach/commit body for a surface; expects buf already created.
// Caller holds surface_mtx.
//
// Hard invariant: this function must never produce subsurface state that
// exceeds the current mpv window size, and must never stretch (src and
// dst rects must scale by the mpv physical/logical ratio). Source clamped
// to min(buf, mpv_pw); destination derived proportionally so the ratio is
// always exactly mpv_pw/mpv_lw — when CEF lags (buf < mpv) the subsurface
// renders smaller than the window, exposing mpv beneath; when CEF overshoots
// (buf > mpv) the buffer is cropped to the top-left mpv-sized region.
// Per-surface s->lw/pw is intentionally not consulted: it lags mpv during
// exactly the race window we care about, and every layer in this
// architecture is sized to the full window.

static void attach_and_commit_locked(PlatformSurface* s, wl_buffer* buf,
                                     int buf_w, int buf_h) {
    if (s->buffer) wl_buffer_destroy(s->buffer);
    s->buffer = buf;
    s->buffer_w = buf_w;
    s->buffer_h = buf_h;
    s->placeholder = false;
    s->null_attached = false;
    if (s->viewport && s->pw > 0 && s->lw > 0) {
        int src_w = std::min(buf_w, s->pw);
        int src_h = std::min(buf_h, s->ph);
        int dst_w = (src_w * s->lw) / s->pw;
        int dst_h = (src_h * s->lh) / s->ph;
        wp_viewport_set_source(s->viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(src_w), wl_fixed_from_int(src_h));
        wp_viewport_set_destination(s->viewport, dst_w, dst_h);
    }
    wl_surface_attach(s->surface, buf, 0, 0);
    wl_surface_damage_buffer(s->surface, 0, 0, buf_w, buf_h);
    wl_surface_commit(s->surface);
    wl_display_flush(g_wl.display);
}

// Paint path is a vtable: g_present swaps between drop (begin-transition
// window, before mpv_pw is updated) and attach (steady).
//
//   begin_transition  : g_present = present_drop
//   on_mpv_configure  : g_present = present_attach (via end_transition)
//
// present_attach passes most buffers through to attach_and_commit_locked
// which clamps non-stretched in all directions:
//   * buf < mpv: src=buf, dst proportional → 1:1 at top-left, gap.
//   * buf == mpv: full window.
//   * buf > mpv: full window, top-left crop.
//
// Exception: during an FS transition (set by begin_transition_locked,
// cleared on first in-tolerance frame), require visible_rect within
// kTransitionToleranceTexels of mpv_pw/ph. FS swaps cause big size
// jumps; rendering a stale-by-far buf 1:1 at top-left or as a top-left
// crop is more jarring than unmapping (mpv shows through the gap)
// until CEF catches up. The 5s nudge loops (rAF in cef_app.cpp +
// Invalidate in cef_client.cpp) drive convergence within the window.
constexpr int kTransitionToleranceTexels = 32;

static bool present_drop(PlatformSurface*, const CefAcceleratedPaintInfo&) { return false; }

static void unmap_locked(PlatformSurface* s) {
    if (!s || !s->surface) return;
    wl_surface_attach(s->surface, nullptr, 0, 0);
    if (s->viewport)
        wp_viewport_set_destination(s->viewport, -1, -1);
    wl_surface_commit(s->surface);
    wl_display_flush(g_wl.display);
    s->null_attached = true;
}

static bool size_in_tolerance_locked(PlatformSurface* s, int vw, int vh) {
    if (!s || s->pw <= 0) return true;
    return std::abs(vw - s->pw) <= kTransitionToleranceTexels &&
           std::abs(vh - s->ph) <= kTransitionToleranceTexels;
}

static bool present_attach(PlatformSurface* s, const CefAcceleratedPaintInfo& info) {
    int w = info.extra.coded_size.width;
    int h = info.extra.coded_size.height;
    int vw = info.extra.visible_rect.width;
    int vh = info.extra.visible_rect.height;
    {
        std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
        if (!s || !s->surface || !s->visible || !g_wl.dmabuf) return false;
        if (g_wl.transitioning && !size_in_tolerance_locked(s, vw, vh)) {
            unmap_locked(s);
            return false;
        }
    }

    auto* buf = create_dmabuf_buffer(info);
    if (!buf) return false;

    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!s->surface || !s->visible) { wl_buffer_destroy(buf); return false; }
    if (g_wl.transitioning && !size_in_tolerance_locked(s, vw, vh)) {
        wl_buffer_destroy(buf);
        unmap_locked(s);
        return false;
    }
    if (g_wl.transitioning) {
        // First in-tolerance frame ends the FS transition.
        g_wl.transitioning = false;
        attach_and_commit_locked(s, buf, w, h);
        return true;
    }
    // Recovery: a previously-null-attached surface (e.g., dropped by gap
    // detect during a transition that never recovered) must attach the
    // first paint it sees, regardless of gate state. Otherwise the
    // subsurface stays unmapped indefinitely.
    if (s->null_attached) {
        attach_and_commit_locked(s, buf, w, h);
        return true;
    }
    // Out-of-tolerance frames don't attach — the previous buffer
    // remains mapped until the renderer catches up to s->pw/ph.
    // Skip-first-N-paints-after-resize lives in CefLayer.
    if (s->pw > 0 && !size_in_tolerance_locked(s, vw, vh)) {
        wl_buffer_destroy(buf);
        return false;
    }
    attach_and_commit_locked(s, buf, w, h);
    return true;
}

static bool (*g_present)(PlatformSurface*, const CefAcceleratedPaintInfo&) = present_attach;

static bool wl_surface_present(PlatformSurface* s, const CefAcceleratedPaintInfo& info) {
    return g_present(s, info);
}

static bool wl_surface_present_software(PlatformSurface* s,
                                        const CefRenderHandler::RectList&,
                                        const void* buffer, int w, int h) {
    auto* buf = create_shm_buffer(buffer, w, h);
    if (!buf) return false;
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!s || !s->surface || !s->visible) { wl_buffer_destroy(buf); return false; }
    attach_and_commit_locked(s, buf, w, h);
    return true;
}

// Push viewport src/dest + commit so the subsurface knows its target
// size before the next paint arrives. src is clamped to the current
// attached buffer's dims (not the new mpv dims) — otherwise the
// compositor samples beyond the buffer and clamp-to-edge repeats the
// last row/column until a fresh paint lands.
static void wl_surface_resize(PlatformSurface* s, int lw, int lh, int pw, int ph) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    s->lw = lw; s->lh = lh; s->pw = pw; s->ph = ph;
    if (!s->surface || !s->viewport) return;
    bool is_main = !g_wl.stack.empty() && s == g_wl.stack[0];
    if (g_wl.transitioning && is_main) {
        // Defer src; dest update is safe.
        wp_viewport_set_destination(s->viewport, lw, lh);
    } else if (s->buffer_w > 0 && s->buffer_h > 0 && pw > 0 && ph > 0) {
        int src_w = std::min(s->buffer_w, pw);
        int src_h = std::min(s->buffer_h, ph);
        int dst_w = (src_w * lw) / pw;
        int dst_h = (src_h * lh) / ph;
        wp_viewport_set_source(s->viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(src_w), wl_fixed_from_int(src_h));
        wp_viewport_set_destination(s->viewport, dst_w, dst_h);
    } else {
        // No buffer yet: just set dst so the next attach has a target.
        wp_viewport_set_destination(s->viewport, lw, lh);
    }
    wl_surface_commit(s->surface);
    wl_display_flush(g_wl.display);
}

static void wl_surface_set_visible(PlatformSurface* s, bool visible) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (s->visible == visible) return;
    s->visible = visible;
    if (!s->surface) return;
    if (visible) {
        // Solid-color placeholder so the user sees the theme background
        // before CEF's first paint lands.
        auto* buf = create_solid_color_buffer(kBgColor);
        if (buf) {
            if (s->buffer) wl_buffer_destroy(s->buffer);
            s->buffer = buf;
            s->placeholder = true;
            if (s->viewport)
                wp_viewport_set_source(s->viewport,
                    wl_fixed_from_int(0), wl_fixed_from_int(0),
                    wl_fixed_from_int(1), wl_fixed_from_int(1));
            wl_surface_attach(s->surface, buf, 0, 0);
            wl_surface_damage_buffer(s->surface, 0, 0, 1, 1);
            wl_surface_commit(s->surface);
            wl_display_flush(g_wl.display);
            s->null_attached = false;
        }
    } else {
        // Reset alpha to fully opaque for next time (post-fade).
        if (s->alpha)
            wp_alpha_modifier_surface_v1_set_multiplier(s->alpha, UINT32_MAX);
        wl_surface_attach(s->surface, nullptr, 0, 0);
        wl_surface_commit(s->surface);
        wl_display_flush(g_wl.display);
        if (s->buffer) { wl_buffer_destroy(s->buffer); s->buffer = nullptr; }
        s->placeholder = false;
        s->null_attached = true;
    }
}

// Per-frame apply, called from the Rust fade thread (jfn_fade.h). Holds
// surface_mtx for the protocol calls so it can't race wl_free_surface or
// wp_alpha_modifier teardown. Returns false to abort the loop when the
// surface state required for the fade has dropped out from under us.
static bool fade_apply_frame(void* surface, uint32_t alpha) {
    auto* s = static_cast<PlatformSurface*>(surface);
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!s->visible || !s->surface || !s->alpha) return false;
    wp_alpha_modifier_surface_v1_set_multiplier(s->alpha, alpha);
    wl_surface_commit(s->surface);
    wl_display_flush(g_wl.display);
    return true;
}

// Box a std::function so it can ride through the C-ABI fade FFI as a
// (fn, ctx, dtor) triple — the Rust side fires it once (or drops it on
// early-skip / abort) and calls the dtor unconditionally.
static void invoke_fn(void* ctx) {
    auto* f = static_cast<std::function<void()>*>(ctx);
    if (*f) (*f)();
}
static void delete_fn(void* ctx) {
    delete static_cast<std::function<void()>*>(ctx);
}

// Animate alpha from opaque to transparent over fade_sec, then hide.
// The thread + stop flag live in jfn_fade; this wrapper only translates the
// std::function callbacks into the FFI triples and routes the per-frame
// protocol calls through fade_apply_frame.
static void wl_fade_surface(PlatformSurface* s, float fade_sec,
                            std::function<void()> on_fade_start,
                            std::function<void()> on_complete) {
    double fps = jfn_playback_display_hz();
    if (!s || !s->alpha || !s->surface || fps <= 0) {
        if (on_fade_start) on_fade_start();
        if (on_complete) on_complete();
        return;
    }

    auto* start_ctx = new std::function<void()>(std::move(on_fade_start));
    auto* done_ctx  = new std::function<void()>(std::move(on_complete));
    jfn_wl_fade_start(s, fade_sec, fps, fade_apply_frame,
                      invoke_fn, start_ctx, delete_fn,
                      invoke_fn, done_ctx,  delete_fn);
}

// =====================================================================
// Popup subsurface (CEF OSR <select> dropdowns).
// =====================================================================

static void wl_popup_show(PlatformSurface* s, const Platform::PopupRequest& req) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    popup_create_locked(s);
    s->popup_visible = true;
    if (!s->popup_subsurface) return;
    wl_subsurface_set_position(s->popup_subsurface, req.x, req.y);
    if (s->popup_viewport && req.lw > 0 && req.lh > 0)
        wp_viewport_set_destination(s->popup_viewport, req.lw, req.lh);
}

static void wl_popup_hide(PlatformSurface* s) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    s->popup_visible = false;
    popup_destroy_locked(s);
    wl_display_flush(g_wl.display);
}

static void wl_popup_present(PlatformSurface* s, const CefAcceleratedPaintInfo& info,
                             int lw, int lh) {
    if (!s || lw <= 0 || lh <= 0) return;
    int w = info.extra.coded_size.width;
    int h = info.extra.coded_size.height;

    auto* buf = create_dmabuf_buffer(info);
    if (!buf) return;

    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!s->popup_surface || !s->popup_visible) {
        wl_buffer_destroy(buf);
        return;
    }
    if (s->popup_buffer) wl_buffer_destroy(s->popup_buffer);
    s->popup_buffer = buf;
    if (s->popup_viewport) {
        wp_viewport_set_source(s->popup_viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(w), wl_fixed_from_int(h));
        wp_viewport_set_destination(s->popup_viewport, lw, lh);
    }
    wl_surface_attach(s->popup_surface, buf, 0, 0);
    wl_surface_damage_buffer(s->popup_surface, 0, 0, w, h);
    // Commit parent (CefLayer surface) first so subsurface state
    // (set_position) lands in the same compositor frame as the popup
    // buffer.
    if (s->surface) wl_surface_commit(s->surface);
    wl_surface_commit(s->popup_surface);
    wl_display_flush(g_wl.display);
}

static void wl_popup_present_software(PlatformSurface* s, const void* buffer,
                                      int pw, int ph, int lw, int lh) {
    if (!s || lw <= 0 || lh <= 0) return;
    auto* buf = create_shm_buffer(buffer, pw, ph);
    if (!buf) return;

    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!s->popup_surface || !s->popup_visible) {
        wl_buffer_destroy(buf);
        return;
    }
    if (s->popup_buffer) wl_buffer_destroy(s->popup_buffer);
    s->popup_buffer = buf;
    if (s->popup_viewport) {
        wp_viewport_set_source(s->popup_viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(pw), wl_fixed_from_int(ph));
        wp_viewport_set_destination(s->popup_viewport, lw, lh);
    }
    wl_surface_attach(s->popup_surface, buf, 0, 0);
    wl_surface_damage_buffer(s->popup_surface, 0, 0, pw, ph);
    if (s->surface) wl_surface_commit(s->surface);
    wl_surface_commit(s->popup_surface);
    wl_display_flush(g_wl.display);
}

// =====================================================================
// Surface alloc / free / restack
// =====================================================================

static PlatformSurface* wl_alloc_surface() {
    auto* s = new PlatformSurface;
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!g_wl.compositor || !g_wl.subcompositor || !g_wl.parent) return s;
    s->surface = wl_compositor_create_surface(g_wl.compositor);
    s->subsurface = wl_subcompositor_get_subsurface(g_wl.subcompositor, s->surface, g_wl.parent);
    wl_subsurface_set_desync(s->subsurface);
    // No input region on subsurface — keystrokes/clicks go to parent only.
    wl_region* empty = wl_compositor_create_region(g_wl.compositor);
    wl_surface_set_input_region(s->surface, empty);
    wl_region_destroy(empty);
    if (g_wl.viewporter)
        s->viewport = wp_viewporter_get_viewport(g_wl.viewporter, s->surface);
    if (g_wl.alpha_modifier)
        s->alpha = wp_alpha_modifier_v1_get_surface(g_wl.alpha_modifier, s->surface);
    wl_surface_commit(s->surface);
    wl_display_flush(g_wl.display);
    return s;
}

// Caller holds surface_mtx. Idempotent — bails if popup already alive.
static void popup_create_locked(PlatformSurface* s) {
    if (!s || !s->surface || !g_wl.compositor || !g_wl.subcompositor) return;
    if (s->popup_surface) return;
    s->popup_surface = wl_compositor_create_surface(g_wl.compositor);
    s->popup_subsurface = wl_subcompositor_get_subsurface(
        g_wl.subcompositor, s->popup_surface, s->surface);
    wl_subsurface_set_desync(s->popup_subsurface);
    wl_region* empty = wl_compositor_create_region(g_wl.compositor);
    wl_surface_set_input_region(s->popup_surface, empty);
    wl_region_destroy(empty);
    if (g_wl.viewporter)
        s->popup_viewport = wp_viewporter_get_viewport(g_wl.viewporter, s->popup_surface);
}

// Caller holds surface_mtx. No-op if popup not alive.
static void popup_destroy_locked(PlatformSurface* s) {
    if (!s) return;
    if (s->popup_viewport) { wp_viewport_destroy(s->popup_viewport); s->popup_viewport = nullptr; }
    if (s->popup_buffer) { wl_buffer_destroy(s->popup_buffer); s->popup_buffer = nullptr; }
    if (s->popup_subsurface) { wl_subsurface_destroy(s->popup_subsurface); s->popup_subsurface = nullptr; }
    if (s->popup_surface) { wl_surface_destroy(s->popup_surface); s->popup_surface = nullptr; }
}

static void wl_free_surface(PlatformSurface* s) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    // Drop from stack if still present (Browsers::remove already updates
    // the vector, but defensive removal keeps state coherent on shutdown).
    auto it = std::find(g_wl.stack.begin(), g_wl.stack.end(), s);
    if (it != g_wl.stack.end()) g_wl.stack.erase(it);
    popup_destroy_locked(s);
    if (s->alpha) wp_alpha_modifier_surface_v1_destroy(s->alpha);
    if (s->viewport) wp_viewport_destroy(s->viewport);
    if (s->buffer) wl_buffer_destroy(s->buffer);
    if (s->subsurface) wl_subsurface_destroy(s->subsurface);
    if (s->surface) wl_surface_destroy(s->surface);
    wl_display_flush(g_wl.display);
    delete s;
}

static void wl_restack(PlatformSurface* const* ordered, size_t n) {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    g_wl.stack.assign(ordered, ordered + n);
    if (!g_wl.parent) return;
    wl_surface* prev = g_wl.parent;
    for (size_t i = 0; i < n; i++) {
        PlatformSurface* s = ordered[i];
        if (!s || !s->subsurface || !s->surface) continue;
        wl_subsurface_place_above(s->subsurface, prev);
        prev = s->surface;
    }
    wl_display_flush(g_wl.display);
}


// =====================================================================
// Registry
// =====================================================================

static void reg_global(void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        g_wl.compositor = static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        g_wl.shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    else if (strcmp(iface, wl_subcompositor_interface.name) == 0)
        g_wl.subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(reg, name, &wl_subcompositor_interface, 1));
    else if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0)
        g_wl.dmabuf = static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, std::min(ver, 4u)));
    else if (strcmp(iface, wp_viewporter_interface.name) == 0)
        g_wl.viewporter = static_cast<wp_viewporter*>(wl_registry_bind(reg, name, &wp_viewporter_interface, 1));
    else if (strcmp(iface, wp_alpha_modifier_v1_interface.name) == 0)
        g_wl.alpha_modifier = static_cast<wp_alpha_modifier_v1*>(wl_registry_bind(reg, name, &wp_alpha_modifier_v1_interface, 1));
    // wl_seat + wp_cursor_shape_manager_v1 are bound by the Rust input layer
    // on its own registry view, not here.
#ifdef HAVE_KDE_DECORATION_PALETTE
    else if (strcmp(iface, org_kde_kwin_server_decoration_palette_manager_interface.name) == 0) {
        g_wl.palette_manager = static_cast<org_kde_kwin_server_decoration_palette_manager*>(
            wl_registry_bind(reg, name, &org_kde_kwin_server_decoration_palette_manager_interface, 1));
    }
#endif
}
static void reg_remove(void*, wl_registry*, uint32_t) {}
static const wl_registry_listener s_reg = { .global = reg_global, .global_remove = reg_remove };

// =====================================================================
// mpv configure callback -- fires from mpv's VO thread
// =====================================================================

// width/height from mpv's geometry are PHYSICAL pixels (already scaled).
//
// Fires from mpv's wayland thread inside handle_toplevel_config — BEFORE
// mpv's xdg_surface ack and before mpv's next render commit on the parent
// surface. This ordering is what makes the hard invariant achievable:
// we get to null-attach our subsurfaces (removing them from the KWin
// bounding box) while mpv's parent is still applied at the old size, so
// when mpv subsequently commits the parent at the new size, our applied
// state is empty.
//
// Trigger transition on either fullscreen toggle OR window shrink. A
// shrink without FS toggle (compositor changed our window size on its
// own, or KWin tile drag) still risks stale-large CEF buffers exceeding
// the new mpv size — same defense applies.
static void on_mpv_configure(void*, int width, int height, bool fs) {
    if (width <= 0 || height <= 0) return;

    int pw = width;
    int ph = height;
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);

    float scale = jfn_wl_get_cached_scale();
    int lw = static_cast<int>(pw / scale);
    int lh = static_cast<int>(ph / scale);

    if (fs != g_wl.was_fullscreen) {
        // Begin if not already (covers WM-initiated FS).
        if (!g_wl.transitioning)
            wl_begin_transition_locked();
        g_wl.was_fullscreen = fs;
    }

    // Fan the configure values out to every surface in the stack. Each
    // CEF layer covers the full window, so they all share these dims.
    // Doing it here (xdg_toplevel.configure callback) leads the slower
    // OSD_DIMS → wl_surface_resize path during FS transitions.
    for (auto* s : g_wl.stack) {
        if (!s) continue;
        s->lw = lw; s->lh = lh; s->pw = pw; s->ph = ph;
    }

    update_surface_size_locked(lw, lh, pw, ph);

    // pw is now NEW. Flip paint gate back to present_attach but keep
    // transitioning=true — present_attach's transition branch will unmap
    // stale-OLD frames and clear transitioning on first matching frame.
    // Restore stack[0] viewport so the first matching frame attaches at
    // the correct src/dst proportional to the new window size.
    if (g_wl.transitioning) {
        g_present = present_attach;
        if (!g_wl.stack.empty()) {
            auto* main = g_wl.stack[0];
            if (main && main->viewport && main->pw > 0 && main->lw > 0) {
                wp_viewport_set_source(main->viewport,
                    wl_fixed_from_int(0), wl_fixed_from_int(0),
                    wl_fixed_from_int(main->pw), wl_fixed_from_int(main->ph));
                wp_viewport_set_destination(main->viewport, main->lw, main->lh);
            }
        }
    }
}

// =====================================================================
// Proxy configure intercept
// =====================================================================

// Fires from the wl-proxy per-client thread for every xdg_toplevel.configure
// from the compositor. Authoritative size source on Wayland: posts to the
// playback ingest layer (which updates osd_pw/osd_ph atomics + drives the
// coordinator), replacing the osd-dimensions observation the rest of the
// codebase used to consume.
//
// Safe before wl_init runs — on_mpv_configure early-outs on empty
// g_wl.stack, and the ingest layer null-checks the coordinator.
extern "C" {
static void on_proxy_configure(int physical_w, int physical_h, int fullscreen) {
    on_mpv_configure(nullptr, physical_w, physical_h, fullscreen != 0);
    if (physical_w > 0 && physical_h > 0) {
        float scale = g_platform.get_scale ? g_platform.get_scale() : 1.0f;
        if (scale <= 0.f) scale = 1.0f;
        jfn_playback_post_osd_pixels(physical_w, physical_h, scale, false, 0, 0);
    }
}
}

namespace platform::wayland {
// Thin shim: hand the Rust side our C++ configure callback. The scale
// callback + cached_scale storage + scale_known() now live in Rust
// (jfn-wayland::proxy).
void register_proxy_callbacks() {
    jfn_wl_register_proxy_callbacks(on_proxy_configure);
}
}

// =====================================================================
// Platform interface
// =====================================================================


static bool wl_init(mpv_handle* mpv) {
    // Seed was_fullscreen from mpv's current state so the first configure
    // after callback registration doesn't start a spurious transition.
    // The main-thread VO-wait loop has already digested mpv's initial
    // fullscreen property-change event, so s_fullscreen is up to date.
    g_wl.was_fullscreen = jfn_playback_fullscreen();

    // Proxy configure + scale callbacks are wired by
    // platform::wayland::register_proxy_callbacks before mpv_create.

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
    auto* parent = reinterpret_cast<wl_surface*>(sp);

    g_wl.display = display;
    g_wl.parent = parent;

    // Dedicated event queue: all our objects live here, isolated from mpv's VO queue
    g_wl.queue = wl_display_create_queue(display);

    // Prepare the input layer so its xkb context is ready before the registry
    // callbacks land (seat_caps wires up keyboard listeners that need xkb).
    input::wayland::init(display);

    auto* reg = wl_display_get_registry(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(reg), g_wl.queue);
    wl_registry_add_listener(reg, &s_reg, nullptr);
    wl_display_roundtrip_queue(display, g_wl.queue);
    wl_registry_destroy(reg);

    if (!g_wl.compositor || !g_wl.subcompositor) {
        LOG_ERROR(LOG_PLATFORM, "platform_wayland: missing compositor globals");
        return false;
    }

    // CefLayer subsurfaces (and their popup children) are allocated
    // on-demand by Browsers via g_platform.alloc_surface/restack.

    wl_display_roundtrip_queue(display, g_wl.queue);

    // Register close callback -- intercepts xdg_toplevel close before mpv sees it
    {
        intptr_t cb_ptr = 0;
        int64_t v = 0;
        if (jfn_mpv_get_property_int("wayland-close-cb-ptr", &v) == 0)
            cb_ptr = static_cast<intptr_t>(v);
        if (cb_ptr) {
            auto* fn = reinterpret_cast<void(**)(void*)>(cb_ptr);
            auto* data = reinterpret_cast<void**>(cb_ptr + sizeof(void*));
            *fn = [](void*) { initiate_shutdown(); };
            *data = nullptr;
        }
    }

    // EGL init for CEF shared texture support + dmabuf probe
    EGLDisplay egl_dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(g_wl.display));
    if (egl_dpy != EGL_NO_DISPLAY) eglInitialize(egl_dpy, nullptr, nullptr);

    if (!jfn_wl_dmabuf_probe(g_platform.cef_ozone_platform.c_str(), egl_dpy)) {
        LOG_WARN(LOG_PLATFORM, "Shared textures not supported; using software CEF rendering");
        g_platform.shared_texture_supported = false;
    }

    // KDE titlebar color — use system theme color until changed by wl_set_theme_color(...)
    wl_init_kde_palette();

    // Start input thread (input layer owns it)
    input::wayland::start_input_thread();

    // Clipboard worker runs on its own wl_display connection + thread and
    // uses ext-data-control-v1. On compositors that don't advertise the
    // protocol (notably Mutter/GNOME) this initializes to a no-op and we
    // clear the Platform hook so the context menu falls back to CEF's
    // native frame->Paste() — Mutter's XWayland clipboard bridge handles
    // external paste correctly there.
    clipboard_wayland::init();
    if (!clipboard_wayland::available())
        g_platform.clipboard_read_text_async = nullptr;

    return true;
}

static float wl_get_scale() {
    return jfn_wl_get_cached_scale();
}

static float wl_get_display_scale(int x, int y) {
    double s = jfn_wayland_scale_probe(x, y);
    return s > 0.0 ? static_cast<float>(s) : 1.0f;
}

static void wl_cleanup() {
    jfn_wl_fade_stop_all();

    // Null the close trampoline we installed into mpv's hook before
    // destroying the g_wl state it reads. It keeps being invoked until mpv
    // itself is torn down, which happens after this function. (The configure
    // hook is no longer used — proxy-side interception replaced it.)
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
    // Clipboard worker owns its own thread + wl_event_queue; must shut
    // down before input::wayland::cleanup destroys the seat it borrowed.
    clipboard_wayland::cleanup();
    // Input layer owns seat/pointer/keyboard/xkb/cursor-shape-device.
    input::wayland::cleanup();
    // Per-layer surfaces (and their popup children) are owned by Browsers
    // and freed via free_surface before cleanup; defensively drop any
    // stragglers.
    for (auto* s : g_wl.stack) wl_free_surface(s);
    g_wl.stack.clear();
    // Globals (must be destroyed before queue — they were bound to it).
    // Cursor shape manager is owned by input::wayland — destroyed in its cleanup.
    if (g_wl.alpha_modifier) { wp_alpha_modifier_v1_destroy(g_wl.alpha_modifier); g_wl.alpha_modifier = nullptr; }
    if (g_wl.shm) { wl_shm_destroy(g_wl.shm); g_wl.shm = nullptr; }
    if (g_wl.dmabuf) { zwp_linux_dmabuf_v1_destroy(g_wl.dmabuf); g_wl.dmabuf = nullptr; }
    if (g_wl.viewporter) { wp_viewporter_destroy(g_wl.viewporter); g_wl.viewporter = nullptr; }
    if (g_wl.subcompositor) { wl_subcompositor_destroy(g_wl.subcompositor); g_wl.subcompositor = nullptr; }
    if (g_wl.compositor) { wl_compositor_destroy(g_wl.compositor); g_wl.compositor = nullptr; }
    if (g_wl.queue) wl_event_queue_destroy(g_wl.queue);
}

// Push a fresh viewport onto cef-main in response to an mpv configure.
// Caller must hold surface_mtx. mpv dims are NOT cached here — every
// reader pulls from jfn_playback_osd_* atomics on demand.
static void update_surface_size_locked(int lw, int lh, int pw, int ph) {
    if (g_wl.stack.empty()) return;
    auto* s = g_wl.stack[0];
    if (!s || !s->surface || !s->viewport) return;
    if (g_wl.transitioning) {
        // During transition: push a dest update on the cef-main layer so
        // its (null-attached) subsurface knows the target size.
        // end_transition applies the final viewport src+dst.
        wp_viewport_set_destination(s->viewport, lw, lh);
        wl_surface_commit(s->surface);
        wl_display_flush(g_wl.display);
        return;
    }
    // Non-transition path: push viewport on cef-main, clamping src to
    // the currently-attached buffer dims (not new mpv dims). Setting src
    // beyond the buffer makes the compositor clamp-to-edge and repeat
    // the last row/col until a fresh paint lands.
    if (s->buffer_w > 0 && s->buffer_h > 0 && pw > 0 && ph > 0) {
        int src_w = std::min(s->buffer_w, pw);
        int src_h = std::min(s->buffer_h, ph);
        int dst_w = (src_w * lw) / pw;
        int dst_h = (src_h * lh) / ph;
        wp_viewport_set_source(s->viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(src_w), wl_fixed_from_int(src_h));
        wp_viewport_set_destination(s->viewport, dst_w, dst_h);
        wl_surface_commit(s->surface);
        wl_display_flush(g_wl.display);
    }
}

// Begin a resize transition. Caller must hold surface_mtx.
//
// Bounding-box release: every CEF subsurface is null-attached with
// destination(-1,-1) and committed in desync mode so the change applies
// immediately, removing the subsurface from KWin's toplevel bounding box
// before mpv's xdg-toplevel reconfigure reaches the compositor. Subsurfaces
// stay in desync — the next CEF paint per layer admits live via
// attach_and_commit_locked with src/dst clamped against the latest
// mpv_pw/lh (the hard no-exceed invariant). Layers that haven't repainted
// yet stay unmapped (mpv shows through the gap) until their next paint
// lands; the gap is acceptable, stretch/oversize is not.
//
// Re-entry safe: a second begin_transition during the same FS toggle just
// re-null-attaches; no cached state to invalidate.
static void wl_begin_transition_locked() {
    g_wl.transitioning = true;
    g_present = present_drop;
    for (auto* s : g_wl.stack) {
        if (!s || !s->surface || !s->subsurface) continue;
        wl_surface_attach(s->surface, nullptr, 0, 0);
        if (s->viewport)
            wp_viewport_set_destination(s->viewport, -1, -1);
        wl_surface_commit(s->surface);
        s->null_attached = true;
    }
    wl_display_flush(g_wl.display);
}

static void wl_end_transition_locked() {
    g_wl.transitioning = false;
    g_present = present_attach;
    if (g_wl.stack.empty()) return;
    auto* s = g_wl.stack[0];
    if (s && s->viewport && s->pw > 0 && s->lw > 0) {
        wp_viewport_set_source(s->viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(s->pw), wl_fixed_from_int(s->ph));
        wp_viewport_set_destination(s->viewport, s->lw, s->lh);
    }
}

static void wl_set_fullscreen(bool fullscreen) {
    // Use g_wl.was_fullscreen (synced from xdg_toplevel.configure via
    // on_mpv_configure) as the current state — no libmpv property involved.
    if (g_wl.was_fullscreen == fullscreen) {
        // Compositor may have rejected our fullscreen change. If we're
        // mid-transition and the state matches the pre-toggle value
        // (was_fullscreen), the compositor forced us back — cancel.
        std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
        if (g_wl.transitioning)
            wl_end_transition_locked();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
        wl_begin_transition_locked();
    }
    jfn_wlproxy_set_fullscreen(fullscreen ? 1 : 0);
}

static void wl_toggle_fullscreen() {
    {
        std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
        wl_begin_transition_locked();
    }
    jfn_wlproxy_set_fullscreen(g_wl.was_fullscreen ? 0 : 1);
}

static void wl_begin_transition() {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    wl_begin_transition_locked();
}

static bool wl_in_transition() {
    return g_wl.transitioning;
}

static void wl_end_transition() {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    wl_end_transition_locked();
}

static void wl_set_expected_size(int, int) {}

static void wl_pump() {}

static void wl_set_idle_inhibit(IdleInhibitLevel level) {
    jfn_idle_inhibit_set(static_cast<uint32_t>(level));
}

// =====================================================================
// KDE titlebar color
// =====================================================================

// The color-scheme template, the file-write logic, and the on-disk lifecycle
// live in Rust (src/wayland/src/kde_palette.rs). This file only owns the
// Wayland protocol bindings; the scheme path is owned by Rust.
#ifdef HAVE_KDE_DECORATION_PALETTE

static void wl_init_kde_palette() {
    if (!g_wl.palette_manager || !g_wl.parent) return;

    g_wl.palette = org_kde_kwin_server_decoration_palette_manager_create(
        g_wl.palette_manager, g_wl.parent);
    if (!g_wl.palette) return;

    if (!jfn_wl_kde_palette_init()) {
        org_kde_kwin_server_decoration_palette_release(g_wl.palette);
        g_wl.palette = nullptr;
        return;
    }
    LOG_INFO(LOG_PLATFORM, "KDE decoration palette ready");
}

static void wl_cleanup_kde_palette() {
    // Don't release the palette object — that tells KWin to drop the per-window
    // override, which makes the titlebar flash back to the system colorscheme
    // while the window is still on-screen during teardown. Let KWin clean it
    // up atomically with the window when the connection drops.
    g_wl.palette = nullptr;
    g_wl.palette_manager = nullptr;
    // colors_path is removed in wl_post_window_cleanup, after mpv tears the
    // window down — KWin may still re-read the file during teardown.
}

static void wl_post_window_cleanup() {
    jfn_wl_kde_palette_post_window_cleanup();
}

static void wl_set_theme_color(const Color& c) {
    LOG_DEBUG(LOG_PLATFORM, "set_theme_color({}) palette={}", c.hex, (void*)g_wl.palette);
    if (!g_wl.palette) return;

    const char* path = jfn_wl_kde_palette_write(c.r, c.g, c.b, c.hex);
    if (!path) return;  // unchanged, uninitialised, or write failed

    org_kde_kwin_server_decoration_palette_set_palette(g_wl.palette, path);
    wl_display_flush(g_wl.display);
    LOG_INFO(LOG_PLATFORM, "set_theme_color({}) applied", c.hex);
}

#else
static void wl_init_kde_palette() {}
static void wl_cleanup_kde_palette() {}
static void wl_post_window_cleanup() {}
static void wl_set_theme_color(const Color&) {}
#endif

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
