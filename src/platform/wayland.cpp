#include "common.h"
#include "cef/cef_client.h"
#include "platform/platform.h"
#include "platform/wayland.h"
#include "platform/wayland_scale_probe.h"
#include "clipboard/wayland.h"
#include "idle_inhibit_linux.h"
#include "open_url_linux.h"
#include "input/input_wayland.h"
#include "mpv/event.h"
#include "wlproxy/wlproxy.h"

#include <wayland-client.h>
#include "linux-dmabuf-v1-client.h"
#include "viewporter-client.h"
#include "alpha-modifier-v1-client.h"
#include "cursor-shape-v1-client.h"
#include <drm/drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <fcntl.h>
#ifdef HAVE_KDE_DECORATION_PALETTE
#include "server-decoration-palette-client.h"
#endif
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
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

    float cached_scale = 0.0f;  // 0 = unknown; wl_get_scale falls back to 1.0
    bool was_fullscreen = false;
    // Resize transition state. transitioning gates non-paint paths
    // (resize, configure, fullscreen reject). Paint path uses a function-
    // pointer swap (g_present) — see present_drop / present_match_or_drop
    // / present_attach.
    bool transitioning = false;

#ifdef HAVE_KDE_DECORATION_PALETTE
    org_kde_kwin_server_decoration_palette_manager* palette_manager = nullptr;
    org_kde_kwin_server_decoration_palette* palette = nullptr;
    std::string colors_dir;
    std::string colors_path;
#endif
};

static WlState g_wl;

// Fade thread state. Joinable so wl_cleanup can stop it before destroying
// the surfaces and alpha modifier it touches — Alt+F4 mid-fade otherwise
// races destruction against the next iteration's set_multiplier/commit.
static std::thread g_fade_thread;
static std::atomic<bool> g_fade_stop{false};

static void stop_fade_thread() {
    if (g_fade_thread.joinable()) {
        g_fade_stop.store(true, std::memory_order_release);
        g_fade_thread.join();
    }
}

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

// Animate alpha from opaque to transparent over fade_sec, then hide.
// Runs on a detached thread — finite UI animation.
static void wl_fade_surface(PlatformSurface* s, float fade_sec,
                            std::function<void()> on_fade_start,
                            std::function<void()> on_complete) {
    if (!s || !s->alpha || !s->surface) {
        if (on_fade_start) on_fade_start();
        if (on_complete) on_complete();
        return;
    }

    stop_fade_thread();
    g_fade_stop.store(false, std::memory_order_release);

    double fps = mpv::display_hz();
    if (fps <= 0) {
        if (on_fade_start) on_fade_start();
        if (on_complete) on_complete();
        return;
    }

    g_fade_thread = std::thread([s, fade_sec, fps,
                 on_fade_start = std::move(on_fade_start),
                 on_complete = std::move(on_complete)]() {
        if (on_fade_start) on_fade_start();

        int total_frames = static_cast<int>(fade_sec * fps);
        if (total_frames < 1) total_frames = 1;
        auto frame_duration = std::chrono::microseconds(static_cast<int64_t>(1e6 / fps));

        bool aborted = false;
        for (int i = 1; i <= total_frames; i++) {
            if (g_fade_stop.load(std::memory_order_acquire)) { aborted = true; break; }
            float t = static_cast<float>(i) / total_frames;
            uint32_t alpha = static_cast<uint32_t>((1.0f - t) * UINT32_MAX);
            {
                std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
                if (!s->visible || !s->surface || !s->alpha) break;
                wp_alpha_modifier_surface_v1_set_multiplier(s->alpha, alpha);
                wl_surface_commit(s->surface);
                wl_display_flush(g_wl.display);
            }
            std::this_thread::sleep_for(frame_duration);
        }

        if (aborted) return;

        if (on_complete) on_complete();
    });
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
    else if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        auto* mgr = static_cast<wp_cursor_shape_manager_v1*>(wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1));
        input::wayland::attach_cursor_shape_manager(mgr);
    }
    else if (strcmp(iface, wl_seat_interface.name) == 0) {
        auto* seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 5u)));
        input::wayland::attach_seat(seat);
    }
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

    float scale = g_wl.cached_scale > 0 ? g_wl.cached_scale : 1.0f;
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
// from the compositor. Authoritative size source on Wayland: updates the
// mpv::osd_pw/osd_ph atomics + posts to the playback coordinator, replacing
// the osd-dimensions observation that the rest of the codebase used to
// consume.
//
// Safe before wl_init runs — on_mpv_configure early-outs on empty
// g_wl.stack, and mpv::set_osd_dims null-checks g_playback_coord.
extern "C" {
static void on_proxy_configure(int physical_w, int physical_h, int fullscreen) {
    on_mpv_configure(nullptr, physical_w, physical_h, fullscreen != 0);
    mpv::set_osd_dims(physical_w, physical_h);
}
static void on_proxy_scale(int scale_120) {
    if (scale_120 > 0)
        g_wl.cached_scale = scale_120 / 120.0f;
}
}

namespace platform::wayland {
bool scale_known() { return g_wl.cached_scale > 0.f; }
void register_proxy_callbacks() {
    // Both callbacks register BEFORE mpv_create so the first compositor
    // configure + preferred_scale events are caught. Otherwise registering
    // them in wl_init misses the initial values — main.cpp computes initial
    // logical dims using g_wl.cached_scale (still 1.0) and CEF overshoots.
    jfn_wlproxy_set_configure_callback(on_proxy_configure);
    jfn_wlproxy_set_scale_callback(on_proxy_scale);
}
}

// =====================================================================
// Platform interface
// =====================================================================

// =====================================================================
// Dmabuf probe -- test GBM → EGL image → GL texture binding on the
// same display type CEF will use (x11 or wayland, per --ozone-platform)
// =====================================================================

// GBM function typedefs for dlsym
struct gbm_device;
struct gbm_bo;
using PFN_gbm_create_device = gbm_device* (*)(int fd);
using PFN_gbm_device_destroy = void (*)(gbm_device*);
using PFN_gbm_bo_create = gbm_bo* (*)(gbm_device*, uint32_t, uint32_t, uint32_t, uint32_t);
using PFN_gbm_bo_destroy = void (*)(gbm_bo*);
using PFN_gbm_bo_get_fd = int (*)(gbm_bo*);
using PFN_gbm_bo_get_stride = uint32_t (*)(gbm_bo*);

// X11 typedefs for dlsym (avoid linking libX11)
using PFN_XOpenDisplay = void* (*)(const char*);
using PFN_XCloseDisplay = int (*)(void*);

// GL constants (avoid GL headers)
#define JFD_GL_TEXTURE_2D 0x0DE1
#define JFD_GL_NO_ERROR   0

// GL function pointer types (resolved via eglGetProcAddress)
using PFN_glGenTextures = void (*)(int, unsigned*);
using PFN_glBindTexture = void (*)(unsigned, unsigned);
using PFN_glDeleteTextures = void (*)(int, const unsigned*);
using PFN_glGetError = unsigned (*)(void);
using PFN_glEGLImageTargetTexture2DOES = void (*)(unsigned, void*);

#ifndef EGL_PLATFORM_X11_KHR
#define EGL_PLATFORM_X11_KHR 0x31D5
#endif

static bool probe_shared_texture_support(const std::string& ozone_platform,
                                         EGLDisplay wayland_egl_dpy) {
    // --- Acquire EGL display matching CEF's ozone platform ---
    EGLDisplay egl_dpy = EGL_NO_DISPLAY;
    bool owns_egl_dpy = false;  // true = we must eglTerminate
    void* x11_dpy = nullptr;
    void* x11_lib = nullptr;
    PFN_XCloseDisplay fn_x11_close = nullptr;

    if (ozone_platform == "wayland") {
        egl_dpy = wayland_egl_dpy;
        LOG_INFO(LOG_PLATFORM, "dmabuf probe: testing on Wayland EGL display");
    } else {
        // Default: x11 — open XWayland connection
        x11_lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!x11_lib) {
            LOG_WARN(LOG_PLATFORM, "dmabuf probe: libX11 not available");
            return false;
        }
        auto fn_open = reinterpret_cast<PFN_XOpenDisplay>(dlsym(x11_lib, "XOpenDisplay"));
        fn_x11_close = reinterpret_cast<PFN_XCloseDisplay>(dlsym(x11_lib, "XCloseDisplay"));
        if (!fn_open || !fn_x11_close) { dlclose(x11_lib); return false; }

        x11_dpy = fn_open(nullptr);
        if (!x11_dpy) {
            LOG_WARN(LOG_PLATFORM, "dmabuf probe: XOpenDisplay failed (no XWayland?)");
            dlclose(x11_lib);
            return false;
        }

        auto fn_get_platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (fn_get_platform)
            egl_dpy = fn_get_platform(EGL_PLATFORM_X11_KHR, x11_dpy, nullptr);
        if (egl_dpy == EGL_NO_DISPLAY)
            egl_dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(x11_dpy));
        if (egl_dpy == EGL_NO_DISPLAY) {
            LOG_WARN(LOG_PLATFORM, "dmabuf probe: no EGL display for X11");
            fn_x11_close(x11_dpy); dlclose(x11_lib);
            return false;
        }
        if (!eglInitialize(egl_dpy, nullptr, nullptr)) {
            LOG_WARN(LOG_PLATFORM, "dmabuf probe: EGL init on X11 failed");
            fn_x11_close(x11_dpy); dlclose(x11_lib);
            return false;
        }
        owns_egl_dpy = true;
        LOG_INFO(LOG_PLATFORM, "dmabuf probe: testing on X11 EGL display");
    }

    if (egl_dpy == EGL_NO_DISPLAY) return false;

    // Cleanup helper — tears down everything we opened
    auto cleanup = [&]() {
        if (owns_egl_dpy) eglTerminate(egl_dpy);
        if (x11_dpy && fn_x11_close) fn_x11_close(x11_dpy);
        if (x11_lib) dlclose(x11_lib);
    };

    // --- Create temporary GLES context for GL texture test ---
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(egl_dpy, cfg_attrs, &config, 1, &num_configs) || num_configs == 0) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: no suitable EGL config");
        cleanup();
        return false;
    }
    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(egl_dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: can't create GLES context");
        cleanup();
        return false;
    }
    EGLint pb_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface pbuf = eglCreatePbufferSurface(egl_dpy, config, pb_attrs);
    bool have_surface = (pbuf != EGL_NO_SURFACE);
    if (!eglMakeCurrent(egl_dpy,
                        have_surface ? pbuf : EGL_NO_SURFACE,
                        have_surface ? pbuf : EGL_NO_SURFACE,
                        ctx)) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: eglMakeCurrent failed");
        if (have_surface) eglDestroySurface(egl_dpy, pbuf);
        eglDestroyContext(egl_dpy, ctx);
        cleanup();
        return false;
    }

    // Cleanup helper for GL context
    auto cleanup_gl = [&]() {
        eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (have_surface) eglDestroySurface(egl_dpy, pbuf);
        eglDestroyContext(egl_dpy, ctx);
    };

    // --- Resolve GL + EGL image functions ---
    auto fn_gen_tex = reinterpret_cast<PFN_glGenTextures>(eglGetProcAddress("glGenTextures"));
    auto fn_bind_tex = reinterpret_cast<PFN_glBindTexture>(eglGetProcAddress("glBindTexture"));
    auto fn_del_tex = reinterpret_cast<PFN_glDeleteTextures>(eglGetProcAddress("glDeleteTextures"));
    auto fn_get_err = reinterpret_cast<PFN_glGetError>(eglGetProcAddress("glGetError"));
    auto fn_img_target = reinterpret_cast<PFN_glEGLImageTargetTexture2DOES>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    auto fn_create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    auto fn_destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));

    if (!fn_gen_tex || !fn_bind_tex || !fn_del_tex || !fn_get_err ||
        !fn_img_target || !fn_create_image || !fn_destroy_image) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: missing GL/EGL image functions");
        cleanup_gl(); cleanup();
        return false;
    }

    // --- Load GBM ---
    void* gbm_lib = dlopen("libgbm.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!gbm_lib) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: libgbm not available, assuming supported");
        cleanup_gl(); cleanup();
        return true;
    }
    auto fn_create_device = reinterpret_cast<PFN_gbm_create_device>(dlsym(gbm_lib, "gbm_create_device"));
    auto fn_device_destroy = reinterpret_cast<PFN_gbm_device_destroy>(dlsym(gbm_lib, "gbm_device_destroy"));
    auto fn_bo_create = reinterpret_cast<PFN_gbm_bo_create>(dlsym(gbm_lib, "gbm_bo_create"));
    auto fn_bo_destroy = reinterpret_cast<PFN_gbm_bo_destroy>(dlsym(gbm_lib, "gbm_bo_destroy"));
    auto fn_bo_get_fd = reinterpret_cast<PFN_gbm_bo_get_fd>(dlsym(gbm_lib, "gbm_bo_get_fd"));
    auto fn_bo_get_stride = reinterpret_cast<PFN_gbm_bo_get_stride>(dlsym(gbm_lib, "gbm_bo_get_stride"));
    if (!fn_create_device || !fn_device_destroy || !fn_bo_create ||
        !fn_bo_destroy || !fn_bo_get_fd || !fn_bo_get_stride) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: libgbm missing symbols, assuming supported");
        dlclose(gbm_lib); cleanup_gl(); cleanup();
        return true;
    }

    // --- Find DRM render node ---
    int drm_fd = -1;
    auto fn_query_display = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDisplayAttribEXT"));
    auto fn_query_device_str = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
        eglGetProcAddress("eglQueryDeviceStringEXT"));
    if (fn_query_display && fn_query_device_str) {
        EGLAttrib device_attrib = 0;
        if (fn_query_display(egl_dpy, EGL_DEVICE_EXT, &device_attrib) && device_attrib) {
            auto egl_device = reinterpret_cast<EGLDeviceEXT>(device_attrib);
            const char* node = fn_query_device_str(egl_device, EGL_DRM_RENDER_NODE_FILE_EXT);
            if (node) {
                drm_fd = open(node, O_RDWR | O_CLOEXEC);
                if (drm_fd >= 0)
                    LOG_INFO(LOG_PLATFORM, "dmabuf probe using render node: {}", node);
            }
        }
    }
    if (drm_fd < 0) {
        for (int i = 128; i < 136; i++) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
            drm_fd = open(path, O_RDWR | O_CLOEXEC);
            if (drm_fd >= 0) break;
        }
    }
    if (drm_fd < 0) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: no DRM render node, assuming supported");
        dlclose(gbm_lib); cleanup_gl(); cleanup();
        return true;
    }

    // --- GBM alloc + dmabuf export ---
    bool result = false;
    gbm_device* device = fn_create_device(drm_fd);
    if (!device) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: gbm_create_device failed");
        close(drm_fd); dlclose(gbm_lib); cleanup_gl(); cleanup();
        return false;
    }

    gbm_bo* bo = fn_bo_create(device, 64, 64, DRM_FORMAT_ARGB8888, 0x0002 /*GBM_BO_USE_RENDERING*/);
    if (!bo) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: gbm_bo_create ARGB8888 failed");
        fn_device_destroy(device); close(drm_fd); dlclose(gbm_lib); cleanup_gl(); cleanup();
        return false;
    }

    int dmabuf_fd = fn_bo_get_fd(bo);
    uint32_t stride = fn_bo_get_stride(bo);
    if (dmabuf_fd < 0) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: gbm_bo_get_fd failed");
        fn_bo_destroy(bo); fn_device_destroy(device);
        close(drm_fd); dlclose(gbm_lib); cleanup_gl(); cleanup();
        return false;
    }

    // --- EGL image import + GL texture binding ---
    EGLint img_attrs[] = {
        EGL_WIDTH, 64,
        EGL_HEIGHT, 64,
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(DRM_FORMAT_ARGB8888),
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(stride),
        EGL_NONE
    };
    EGLImageKHR image = fn_create_image(egl_dpy, EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT, nullptr, img_attrs);
    if (!image) {
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: eglCreateImageKHR failed (0x{:x})", eglGetError());
    } else {
        // Full GL texture binding test — this is the step that fails on
        // affected systems and matches Chromium's Skia Ganesh code path.
        unsigned tex = 0;
        fn_gen_tex(1, &tex);
        fn_bind_tex(JFD_GL_TEXTURE_2D, tex);
        fn_img_target(JFD_GL_TEXTURE_2D, image);
        unsigned err = fn_get_err();
        if (err == JFD_GL_NO_ERROR) {
            result = true;
        } else {
            LOG_WARN(LOG_PLATFORM, "dmabuf probe: glEGLImageTargetTexture2DOES failed (0x{:x})", err);
        }
        fn_del_tex(1, &tex);
        fn_destroy_image(egl_dpy, image);
    }
    close(dmabuf_fd);

    fn_bo_destroy(bo);
    fn_device_destroy(device);
    close(drm_fd);
    dlclose(gbm_lib);
    cleanup_gl();
    cleanup();

    if (result)
        LOG_INFO(LOG_PLATFORM, "dmabuf probe: GBM -> EGL -> GL import OK");
    else
        LOG_WARN(LOG_PLATFORM, "dmabuf probe: ARGB8888 dmabuf import failed");

    return result;
}

static bool wl_init(mpv_handle* mpv) {
    // Seed was_fullscreen from mpv's current state so the first configure
    // after callback registration doesn't start a spurious transition.
    // The main-thread VO-wait loop has already digested mpv's initial
    // fullscreen property-change event, so s_fullscreen is up to date.
    g_wl.was_fullscreen = mpv::fullscreen();

    // Proxy configure + scale callbacks are wired by
    // platform::wayland::register_proxy_callbacks before mpv_create.

    intptr_t dp = 0, sp = 0;
    g_mpv.GetWaylandDisplay(dp);
    g_mpv.GetWaylandSurface(sp);
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
    input::wayland::init(display, g_wl.queue);

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
        g_mpv.GetWaylandCloseCbPtr(cb_ptr);
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

    if (!probe_shared_texture_support(g_platform.cef_ozone_platform, egl_dpy)) {
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
    // g_wl.cached_scale is driven by jfn_wlproxy_set_scale_callback (registered
    // in wl_init). Falls back to 1.0 before the compositor sends preferred_scale.
    return g_wl.cached_scale > 0 ? g_wl.cached_scale : 1.0f;
}

static float wl_get_display_scale(int x, int y) {
    double s = wayland_scale_probe::query_scale(x, y);
    return s > 0.0 ? static_cast<float>(s) : 1.0f;
}

static void wl_cleanup() {
    stop_fade_thread();

    // Null the close trampoline we installed into mpv's hook before
    // destroying the g_wl state it reads. It keeps being invoked until mpv
    // itself is torn down, which happens after this function. (The configure
    // hook is no longer used — proxy-side interception replaced it.)
    {
        intptr_t cb_ptr = 0;
        g_mpv.GetWaylandCloseCbPtr(cb_ptr);
        if (cb_ptr) {
            auto* fn = reinterpret_cast<void(**)(void*)>(cb_ptr);
            *fn = nullptr;
        }
    }

    wl_cleanup_kde_palette();
    idle_inhibit::cleanup();
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
// reader pulls from mpv::osd_* atomics on demand.
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
    idle_inhibit::set(level);
}

// =====================================================================
// KDE titlebar color
// =====================================================================

#ifdef HAVE_KDE_DECORATION_PALETTE

// Base color scheme template (derived from BreezeDark).
// Placeholders substituted at runtime: %HEADER_BG%, %INACTIVE_BG%, %ACTIVE_FG%, %INACTIVE_FG%.
static constexpr const char* kColorSchemeTemplate = R"([ColorEffects:Disabled]
Color=56,56,56
ColorAmount=0
ColorEffect=0
ContrastAmount=0.65
ContrastEffect=1
IntensityAmount=0.1
IntensityEffect=2

[ColorEffects:Inactive]
ChangeSelectionColor=true
Color=112,111,110
ColorAmount=0.025
ColorEffect=2
ContrastAmount=0.1
ContrastEffect=2
Enable=false
IntensityAmount=0
IntensityEffect=0

[Colors:Button]
BackgroundAlternate=30,87,116
BackgroundNormal=41,44,48
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=252,252,252
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:Complementary]
BackgroundAlternate=30,87,116
BackgroundNormal=32,35,38
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=252,252,252
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:Header]
BackgroundAlternate=%HEADER_BG%
BackgroundNormal=%HEADER_BG%
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=%ACTIVE_FG%
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:Header][Inactive]
BackgroundAlternate=%INACTIVE_BG%
BackgroundNormal=%INACTIVE_BG%
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=%INACTIVE_FG%
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:Selection]
BackgroundAlternate=30,87,116
BackgroundNormal=61,174,233
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=252,252,252
ForegroundInactive=161,169,177
ForegroundLink=253,188,75
ForegroundNegative=176,55,69
ForegroundNeutral=198,92,0
ForegroundNormal=252,252,252
ForegroundPositive=23,104,57
ForegroundVisited=155,89,182

[Colors:Tooltip]
BackgroundAlternate=32,35,38
BackgroundNormal=41,44,48
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=252,252,252
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:View]
BackgroundAlternate=29,31,34
BackgroundNormal=20,22,24
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=252,252,252
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[Colors:Window]
BackgroundAlternate=41,44,48
BackgroundNormal=32,35,38
DecorationFocus=61,174,233
DecorationHover=61,174,233
ForegroundActive=61,174,233
ForegroundInactive=161,169,177
ForegroundLink=29,153,243
ForegroundNegative=218,68,83
ForegroundNeutral=246,116,0
ForegroundNormal=252,252,252
ForegroundPositive=39,174,96
ForegroundVisited=155,89,182

[KDE]
contrast=4

[WM]
activeBackground=%HEADER_BG%
activeBlend=252,252,252
activeForeground=%ACTIVE_FG%
inactiveBackground=%INACTIVE_BG%
inactiveBlend=161,169,177
inactiveForeground=%INACTIVE_FG%

[General]
ColorScheme=JellyfinDesktop
Name=Jellyfin Desktop
)";

static void replaceAll(std::string& s, const char* token, const char* value) {
    size_t tlen = strlen(token), vlen = strlen(value), pos = 0;
    while ((pos = s.find(token, pos)) != std::string::npos) {
        s.replace(pos, tlen, value);
        pos += vlen;
    }
}

static bool writeColorScheme(const Color& c, const std::string& path) {
    char bg[32];
    snprintf(bg, sizeof(bg), "%d,%d,%d", c.r, c.g, c.b);

    // BT.709 luminance — choose readable foreground
    double lum = 0.2126 * (c.r / 255.0) + 0.7152 * (c.g / 255.0) + 0.0722 * (c.b / 255.0);
    const char* active_fg   = lum < 0.5 ? "252,252,252" : "35,38,41";
    const char* inactive_fg = lum < 0.5 ? "126,126,126" : "35,38,41";

    std::string content(kColorSchemeTemplate);
    replaceAll(content, "%HEADER_BG%", bg);
    replaceAll(content, "%INACTIVE_BG%", bg);
    replaceAll(content, "%ACTIVE_FG%", active_fg);
    replaceAll(content, "%INACTIVE_FG%", inactive_fg);

    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
    fclose(f);
    if (!ok) remove(path.c_str());
    return ok;
}

static void wl_init_kde_palette() {
    if (!g_wl.palette_manager || !g_wl.parent) return;

    g_wl.palette = org_kde_kwin_server_decoration_palette_manager_create(
        g_wl.palette_manager, g_wl.parent);
    if (!g_wl.palette) return;

    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !runtime[0]) {
        org_kde_kwin_server_decoration_palette_release(g_wl.palette);
        g_wl.palette = nullptr;
        return;
    }
    g_wl.colors_dir = std::string(runtime) + "/jellyfin-desktop";
    mkdir(g_wl.colors_dir.c_str(), 0700);
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
    if (!g_wl.colors_path.empty()) {
        remove(g_wl.colors_path.c_str());
        g_wl.colors_path.clear();
    }
}

static void wl_set_theme_color(const Color& c) {
    LOG_DEBUG(LOG_PLATFORM, "set_theme_color({}) palette={}", c.hex, (void*)g_wl.palette);
    if (!g_wl.palette) return;

    char filename[64];
    snprintf(filename, sizeof(filename), "JellyfinDesktop-%s.colors", c.hex + 1);  // skip leading '#'
    std::string new_path = g_wl.colors_dir + "/" + filename;
    if (new_path == g_wl.colors_path) return;

    if (!writeColorScheme(c, new_path)) return;

    if (!g_wl.colors_path.empty())
        remove(g_wl.colors_path.c_str());
    g_wl.colors_path = new_path;

    org_kde_kwin_server_decoration_palette_set_palette(g_wl.palette, g_wl.colors_path.c_str());
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
        .open_external_url = open_url_linux::open,
    };
}
