#include "common.h"
#include "cef/cef_client.h"
#include "browser/browsers.h"
#include "browser/web_browser.h"
#include "browser/overlay_browser.h"
#include "clipboard/wayland.h"
#include "idle_inhibit_linux.h"
#include "open_url_linux.h"
#include "input/input_wayland.h"

#include <wayland-client.h>
#include "linux-dmabuf-v1-client.h"
#include "viewporter-client.h"
#include "alpha-modifier-v1-client.h"
#include "cursor-shape-v1-client.h"
// Callback fields in mpv's vo_wayland_state -- set via wayland-state property.
// Must match the struct layout in wayland_common.h.
struct wl_configure_cb {
    void (*fn)(void *data, int width, int height, bool fullscreen);
    void *data;
};
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
#include <mutex>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include "logging.h"


// =====================================================================
// Wayland state (file-static)
// =====================================================================

struct WlState {
    std::mutex surface_mtx;  // protects surface ops between CEF thread and VO thread
    wl_display* display = nullptr;
    wl_event_queue* queue = nullptr;  // dedicated queue, isolated from mpv's
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_surface* parent = nullptr;

    // Main browser subsurface
    wl_surface* cef_surface = nullptr;
    wl_subsurface* cef_subsurface = nullptr;
    wl_buffer* cef_buffer = nullptr;
    wp_viewport* cef_viewport = nullptr;

    // Overlay browser subsurface (above main)
    wl_surface* overlay_surface = nullptr;
    wl_subsurface* overlay_subsurface = nullptr;
    wl_buffer* overlay_buffer = nullptr;
    wp_viewport* overlay_viewport = nullptr;
    bool overlay_visible = false;
    bool overlay_placeholder = false;  // true while showing solid-color placeholder

    // Popup subsurface (CEF OSR popup elements, e.g. <select> dropdowns)
    wl_surface* popup_surface = nullptr;
    wl_subsurface* popup_subsurface = nullptr;
    wl_buffer* popup_buffer = nullptr;
    wp_viewport* popup_viewport = nullptr;
    bool popup_visible = false;

    // Shared globals
    wl_shm* shm = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_alpha_modifier_v1* alpha_modifier = nullptr;
    wp_alpha_modifier_surface_v1* overlay_alpha = nullptr;

    float cached_scale = 1.0f;
    int mpv_pw = 0, mpv_ph = 0;      // mpv's current physical size
    int transition_pw = 0, transition_ph = 0;
    int pending_lw = 0, pending_lh = 0;
    int expected_w = 0, expected_h = 0;
    bool transitioning = false;
    bool was_fullscreen = false;

#ifdef HAVE_KDE_DECORATION_PALETTE
    org_kde_kwin_server_decoration_palette_manager* palette_manager = nullptr;
    org_kde_kwin_server_decoration_palette* palette = nullptr;
    std::string colors_dir;
    std::string colors_path;
#endif
};

static WlState g_wl;

static void update_surface_size_locked(int lw, int lh, int pw, int ph);
static void wl_begin_transition_locked();
static void wl_end_transition_locked();
static void wl_set_expected_size_locked(int w, int h);
static void wl_begin_transition();
static void wl_toggle_fullscreen();
static void wl_init_kde_palette();
static void wl_cleanup_kde_palette();
static void wl_set_titlebar_color(uint8_t r, uint8_t g, uint8_t b);

// Create a 1x1 ARGB8888 wl_buffer filled with a solid color.
// Uses an anonymous shm fd — the buffer is self-contained.
static wl_buffer* create_solid_color_buffer(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_wl.shm) return nullptr;
    const int stride = 4, size = stride;  // 1x1 pixel, 4 bytes
    int fd = memfd_create("solid-color", MFD_CLOEXEC);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, size) < 0) { close(fd); return nullptr; }
    auto* data = static_cast<uint8_t*>(mmap(nullptr, size, PROT_WRITE, MAP_SHARED, fd, 0));
    if (data == MAP_FAILED) { close(fd); return nullptr; }
    // ARGB8888: [B, G, R, A]
    data[0] = b; data[1] = g; data[2] = r; data[3] = 0xFF;
    munmap(data, size);
    auto* pool = wl_shm_create_pool(g_wl.shm, fd, size);
    auto* buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

// =====================================================================
// Present CEF dmabuf -- main browser
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

static void wl_present(const CefAcceleratedPaintInfo& info) {
    int w = info.extra.coded_size.width;
    int h = info.extra.coded_size.height;

    // Phase 1: check if we should drop this frame
    {
        std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
        if (!g_wl.cef_surface || !g_wl.dmabuf) return;
        if (g_wl.transitioning) {
            if (g_wl.expected_w <= 0 || (w == g_wl.transition_pw && h == g_wl.transition_ph))
                return;
        }
    }

    // Phase 2: create dmabuf buffer (expensive, no lock)
    auto* buf = create_dmabuf_buffer(info);
    if (!buf) return;

    // Phase 3: attach + commit under lock
    {
        std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
        if (!g_wl.cef_surface) { wl_buffer_destroy(buf); return; }
        // Drop oversized buffers
        if (g_wl.mpv_pw > 0 && (w > g_wl.mpv_pw + 2 || h > g_wl.mpv_ph + 2)) {
            wl_buffer_destroy(buf);
            return;
        }
        if (g_wl.transitioning) {
            if (g_wl.expected_w <= 0 || (w == g_wl.transition_pw && h == g_wl.transition_ph)) {
                wl_buffer_destroy(buf);
                return;
            }
            wl_end_transition_locked();
        }

        if (g_wl.cef_buffer) wl_buffer_destroy(g_wl.cef_buffer);
        g_wl.cef_buffer = buf;
        if (g_wl.cef_viewport && g_wl.mpv_pw > 0) {
            // Crop source to the smaller of buffer vs mpv window
            int cw = w < g_wl.mpv_pw ? w : g_wl.mpv_pw;
            int ch = h < g_wl.mpv_ph ? h : g_wl.mpv_ph;
            float scale = g_wl.cached_scale > 0 ? g_wl.cached_scale : 1.0f;
            wp_viewport_set_source(g_wl.cef_viewport,
                wl_fixed_from_int(0), wl_fixed_from_int(0),
                wl_fixed_from_int(cw), wl_fixed_from_int(ch));
            // Destination must match source at 1:1 pixels — never stretch.
            // When buffer matches mpv size, this fills the window.
            // When buffer is smaller (stale frame after resize), this shows
            // the buffer at correct size with video visible in the gap.
            wp_viewport_set_destination(g_wl.cef_viewport,
                static_cast<int>(cw / scale),
                static_cast<int>(ch / scale));
        }
        wl_surface_attach(g_wl.cef_surface, buf, 0, 0);
        wl_surface_damage_buffer(g_wl.cef_surface, 0, 0, w, h);
        wl_surface_commit(g_wl.cef_surface);
        wl_display_flush(g_wl.display);
    }
}

// =====================================================================
// Present CEF dmabuf -- overlay browser
// =====================================================================

static void wl_overlay_present(const CefAcceleratedPaintInfo& info) {
    int w = info.extra.coded_size.width;
    int h = info.extra.coded_size.height;

    auto* buf = create_dmabuf_buffer(info);
    if (!buf) return;

    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!g_wl.overlay_surface || !g_wl.overlay_visible) {
        wl_buffer_destroy(buf);
        return;
    }

    if (g_wl.overlay_buffer) wl_buffer_destroy(g_wl.overlay_buffer);
    g_wl.overlay_buffer = buf;
    g_wl.overlay_placeholder = false;
    if (g_wl.overlay_viewport && g_wl.mpv_pw > 0) {
        int cw = w < g_wl.mpv_pw ? w : g_wl.mpv_pw;
        int ch = h < g_wl.mpv_ph ? h : g_wl.mpv_ph;
        float scale = g_wl.cached_scale > 0 ? g_wl.cached_scale : 1.0f;
        wp_viewport_set_source(g_wl.overlay_viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(cw), wl_fixed_from_int(ch));
        wp_viewport_set_destination(g_wl.overlay_viewport,
            static_cast<int>(cw / scale),
            static_cast<int>(ch / scale));
    }
    wl_surface_attach(g_wl.overlay_surface, buf, 0, 0);
    wl_surface_damage_buffer(g_wl.overlay_surface, 0, 0, w, h);
    wl_surface_commit(g_wl.overlay_surface);
    wl_display_flush(g_wl.display);
}

// =====================================================================
// Software present (wl_shm fallback when shared textures unavailable)
// =====================================================================

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

static void wl_present_software(const CefRenderHandler::RectList&,
                                const void* buffer, int w, int h) {
    auto* buf = create_shm_buffer(buffer, w, h);
    if (!buf) return;

    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!g_wl.cef_surface) { wl_buffer_destroy(buf); return; }
    if (g_wl.mpv_pw > 0 && (w > g_wl.mpv_pw + 2 || h > g_wl.mpv_ph + 2)) {
        wl_buffer_destroy(buf);
        return;
    }

    if (g_wl.cef_buffer) wl_buffer_destroy(g_wl.cef_buffer);
    g_wl.cef_buffer = buf;
    if (g_wl.cef_viewport && g_wl.mpv_pw > 0) {
        int cw = w < g_wl.mpv_pw ? w : g_wl.mpv_pw;
        int ch = h < g_wl.mpv_ph ? h : g_wl.mpv_ph;
        float scale = g_wl.cached_scale > 0 ? g_wl.cached_scale : 1.0f;
        wp_viewport_set_source(g_wl.cef_viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(cw), wl_fixed_from_int(ch));
        wp_viewport_set_destination(g_wl.cef_viewport,
            static_cast<int>(cw / scale),
            static_cast<int>(ch / scale));
    }
    wl_surface_attach(g_wl.cef_surface, buf, 0, 0);
    wl_surface_damage_buffer(g_wl.cef_surface, 0, 0, w, h);
    wl_surface_commit(g_wl.cef_surface);
    wl_display_flush(g_wl.display);
}

static void wl_overlay_present_software(const CefRenderHandler::RectList&,
                                        const void* buffer, int w, int h) {
    auto* buf = create_shm_buffer(buffer, w, h);
    if (!buf) return;

    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!g_wl.overlay_surface || !g_wl.overlay_visible) {
        wl_buffer_destroy(buf);
        return;
    }

    if (g_wl.overlay_buffer) wl_buffer_destroy(g_wl.overlay_buffer);
    g_wl.overlay_buffer = buf;
    g_wl.overlay_placeholder = false;
    if (g_wl.overlay_viewport && g_wl.mpv_pw > 0) {
        int cw = w < g_wl.mpv_pw ? w : g_wl.mpv_pw;
        int ch = h < g_wl.mpv_ph ? h : g_wl.mpv_ph;
        float scale = g_wl.cached_scale > 0 ? g_wl.cached_scale : 1.0f;
        wp_viewport_set_source(g_wl.overlay_viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(cw), wl_fixed_from_int(ch));
        wp_viewport_set_destination(g_wl.overlay_viewport,
            static_cast<int>(cw / scale),
            static_cast<int>(ch / scale));
    }
    wl_surface_attach(g_wl.overlay_surface, buf, 0, 0);
    wl_surface_damage_buffer(g_wl.overlay_surface, 0, 0, w, h);
    wl_surface_commit(g_wl.overlay_surface);
    wl_display_flush(g_wl.display);
}

// =====================================================================
// Popup subsurface (CEF OSR popup elements)
// =====================================================================

static void wl_popup_show(int x, int y, int lw, int lh) {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    g_wl.popup_visible = true;
    if (!g_wl.popup_subsurface) return;
    wl_subsurface_set_position(g_wl.popup_subsurface, x, y);
    if (g_wl.popup_viewport && lw > 0 && lh > 0)
        wp_viewport_set_destination(g_wl.popup_viewport, lw, lh);
}

static void wl_popup_hide() {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    g_wl.popup_visible = false;
    if (!g_wl.popup_surface) return;
    wl_surface_attach(g_wl.popup_surface, nullptr, 0, 0);
    wl_surface_commit(g_wl.popup_surface);
    wl_display_flush(g_wl.display);
    if (g_wl.popup_buffer) {
        wl_buffer_destroy(g_wl.popup_buffer);
        g_wl.popup_buffer = nullptr;
    }
}

static void wl_popup_present(const CefAcceleratedPaintInfo& info, int lw, int lh) {
    if (lw <= 0 || lh <= 0) return;
    int w = info.extra.coded_size.width;
    int h = info.extra.coded_size.height;

    auto* buf = create_dmabuf_buffer(info);
    if (!buf) return;

    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!g_wl.popup_surface || !g_wl.popup_visible) {
        wl_buffer_destroy(buf);
        return;
    }
    if (g_wl.popup_buffer) wl_buffer_destroy(g_wl.popup_buffer);
    g_wl.popup_buffer = buf;
    if (g_wl.popup_viewport) {
        wp_viewport_set_source(g_wl.popup_viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(w), wl_fixed_from_int(h));
        wp_viewport_set_destination(g_wl.popup_viewport, lw, lh);
    }
    wl_surface_attach(g_wl.popup_surface, buf, 0, 0);
    wl_surface_damage_buffer(g_wl.popup_surface, 0, 0, w, h);
    // Commit cef_surface first so set_position takes effect in the
    // same compositor frame as the popup buffer.
    wl_surface_commit(g_wl.cef_surface);
    wl_surface_commit(g_wl.popup_surface);
    wl_display_flush(g_wl.display);
}

static void wl_popup_present_software(const void* buffer, int pw, int ph, int lw, int lh) {
    if (lw <= 0 || lh <= 0) return;
    auto* buf = create_shm_buffer(buffer, pw, ph);
    if (!buf) return;

    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!g_wl.popup_surface || !g_wl.popup_visible) {
        wl_buffer_destroy(buf);
        return;
    }
    if (g_wl.popup_buffer) wl_buffer_destroy(g_wl.popup_buffer);
    g_wl.popup_buffer = buf;
    if (g_wl.popup_viewport) {
        wp_viewport_set_source(g_wl.popup_viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(pw), wl_fixed_from_int(ph));
        wp_viewport_set_destination(g_wl.popup_viewport, lw, lh);
    }
    wl_surface_attach(g_wl.popup_surface, buf, 0, 0);
    wl_surface_damage_buffer(g_wl.popup_surface, 0, 0, pw, ph);
    wl_surface_commit(g_wl.cef_surface);
    wl_surface_commit(g_wl.popup_surface);
    wl_display_flush(g_wl.display);
}

static void wl_overlay_resize(int lw, int lh, int pw, int ph) {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!g_wl.overlay_surface || !g_wl.overlay_viewport) return;
    // Don't update source while placeholder is active — it's 1x1, not pw×ph.
    // Destination is safe to update either way.
    if (!g_wl.overlay_placeholder)
        wp_viewport_set_source(g_wl.overlay_viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(pw), wl_fixed_from_int(ph));
    wp_viewport_set_destination(g_wl.overlay_viewport, lw, lh);
    wl_surface_commit(g_wl.overlay_surface);
    wl_display_flush(g_wl.display);
}

static void wl_set_overlay_visible(bool visible) {
    {
        std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
        if (g_wl.overlay_visible == visible) return;
        g_wl.overlay_visible = visible;
        if (!g_wl.overlay_surface) return;
        if (visible) {
            // Attach a solid placeholder so the user sees the correct
            // background color immediately, before CEF renders its first frame.
            auto* buf = create_solid_color_buffer(kBgColor.r, kBgColor.g, kBgColor.b);
            if (buf) {
                if (g_wl.overlay_buffer) wl_buffer_destroy(g_wl.overlay_buffer);
                g_wl.overlay_buffer = buf;
                g_wl.overlay_placeholder = true;
                // Source covers the whole 1x1 buffer; destination set by overlay_resize.
                if (g_wl.overlay_viewport)
                    wp_viewport_set_source(g_wl.overlay_viewport,
                        wl_fixed_from_int(0), wl_fixed_from_int(0),
                        wl_fixed_from_int(1), wl_fixed_from_int(1));
                wl_surface_attach(g_wl.overlay_surface, buf, 0, 0);
                wl_surface_damage_buffer(g_wl.overlay_surface, 0, 0, 1, 1);
                wl_surface_commit(g_wl.overlay_surface);
                wl_display_flush(g_wl.display);
            }
        } else {
            // Reset alpha to fully opaque for next time
            if (g_wl.overlay_alpha) {
                wp_alpha_modifier_surface_v1_set_multiplier(g_wl.overlay_alpha, UINT32_MAX);
            }
            wl_surface_attach(g_wl.overlay_surface, nullptr, 0, 0);
            wl_surface_commit(g_wl.overlay_surface);
            wl_display_flush(g_wl.display);
            if (g_wl.overlay_buffer) {
                wl_buffer_destroy(g_wl.overlay_buffer);
                g_wl.overlay_buffer = nullptr;
            }
            g_wl.overlay_placeholder = false;
        }
    }

    // Route keyboard focus to the newly-active browser. Without this, CEF
    // thinks the just-activated browser has no window focus, so text inputs
    // don't show a caret and focus rings don't render. Matches the "active
    // tab" semantics: only one browser at a time holds focus.
    auto main = g_web_browser ? g_web_browser->browser() : nullptr;
    auto ovl  = g_overlay_browser ? g_overlay_browser->browser() : nullptr;
    if (visible) {
        if (main) main->GetHost()->SetFocus(false);
        if (ovl)  ovl->GetHost()->SetFocus(true);
    } else {
        if (ovl)  ovl->GetHost()->SetFocus(false);
        if (main) main->GetHost()->SetFocus(true);
    }
}

// Animate overlay alpha from opaque to transparent over fade_sec, then hide.
// Runs on a detached thread — finite UI animation.
static void wl_fade_overlay(float fade_sec,
                            std::function<void()> on_fade_start,
                            std::function<void()> on_complete) {
    if (!g_wl.overlay_alpha || !g_wl.overlay_surface) {
        // No alpha modifier support — just hide immediately
        wl_set_overlay_visible(false);
        if (on_fade_start) on_fade_start();
        if (on_complete) on_complete();
        return;
    }

    std::thread([fade_sec,
                 on_fade_start = std::move(on_fade_start),
                 on_complete = std::move(on_complete)]() {
        if (on_fade_start) on_fade_start();

        int fps = g_display_hz.load(std::memory_order_relaxed);
        int total_frames = static_cast<int>(fade_sec * fps);
        if (total_frames < 1) total_frames = 1;
        auto frame_duration = std::chrono::microseconds(1000000 / fps);

        for (int i = 1; i <= total_frames; i++) {
            float t = static_cast<float>(i) / total_frames;
            uint32_t alpha = static_cast<uint32_t>((1.0f - t) * UINT32_MAX);

            {
                std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
                if (!g_wl.overlay_visible || !g_wl.overlay_surface) break;
                wp_alpha_modifier_surface_v1_set_multiplier(g_wl.overlay_alpha, alpha);
                wl_surface_commit(g_wl.overlay_surface);
                wl_display_flush(g_wl.display);
            }
            std::this_thread::sleep_for(frame_duration);
        }

        wl_set_overlay_visible(false);
        if (on_complete) on_complete();
    }).detach();
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
static void on_mpv_configure(void*, int width, int height, bool fs) {
    if (width <= 0 || height <= 0) return;

    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);

    int pw = width;
    int ph = height;
    float scale = g_wl.cached_scale > 0 ? g_wl.cached_scale : 1.0f;
    int lw = static_cast<int>(pw / scale);
    int lh = static_cast<int>(ph / scale);

    if (fs != g_wl.was_fullscreen) {
        if (!g_wl.transitioning) {
            wl_begin_transition_locked();
            // Set expected size so the transition can end as soon as a
            // correctly-sized frame arrives, without waiting for an
            // OSD_DIMS event (which the init loop may have consumed).
            wl_set_expected_size_locked(pw, ph);
        } else {
            wl_end_transition_locked();
        }
        g_wl.was_fullscreen = fs;
    }

    update_surface_size_locked(lw, lh, pw, ph);
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
        LOG_INFO(LOG_PLATFORM, "dmabuf probe: libgbm not available, assuming supported");
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
    {
        bool fs = false;
        g_mpv.GetFullscreen(fs);
        g_wl.was_fullscreen = fs;
    }

    // Register mpv configure callback early — mpv's VO thread is already
    // processing configures in parallel, and we need to catch them all.
    // on_mpv_configure is safe before surfaces exist (null checks throughout).
    {
        intptr_t cb_ptr = 0;
        g_mpv.GetWaylandConfigureCbPtr(cb_ptr);
        if (cb_ptr) {
            auto* fn = reinterpret_cast<void(**)(void*, int, int, bool)>(cb_ptr);
            auto* data = reinterpret_cast<void**>(cb_ptr + sizeof(void*));
            *fn = [](void*, int w, int h, bool fs) { on_mpv_configure(nullptr, w, h, fs); };
            *data = nullptr;
        }
    }

    intptr_t dp = 0, sp = 0;
    g_mpv.GetWaylandDisplay(dp);
    g_mpv.GetWaylandSurface(sp);
    if (!dp || !sp) {
        fprintf(stderr, "Failed to get Wayland display/surface from mpv\n");
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
        fprintf(stderr, "platform_wayland: missing compositor globals\n");
        return false;
    }

    // --- Main browser subsurface (above mpv parent) ---
    g_wl.cef_surface = wl_compositor_create_surface(g_wl.compositor);
    g_wl.cef_subsurface = wl_subcompositor_get_subsurface(g_wl.subcompositor, g_wl.cef_surface, parent);
    wl_subsurface_place_above(g_wl.cef_subsurface, parent);
    wl_subsurface_set_desync(g_wl.cef_subsurface);
    {
        wl_region* empty = wl_compositor_create_region(g_wl.compositor);
        wl_surface_set_input_region(g_wl.cef_surface, empty);
        wl_region_destroy(empty);
    }
    if (g_wl.viewporter)
        g_wl.cef_viewport = wp_viewporter_get_viewport(g_wl.viewporter, g_wl.cef_surface);
    wl_surface_commit(g_wl.cef_surface);

    // --- Overlay browser subsurface (above main CEF) ---
    g_wl.overlay_surface = wl_compositor_create_surface(g_wl.compositor);
    g_wl.overlay_subsurface = wl_subcompositor_get_subsurface(g_wl.subcompositor, g_wl.overlay_surface, parent);
    wl_subsurface_place_above(g_wl.overlay_subsurface, g_wl.cef_surface);
    wl_subsurface_set_desync(g_wl.overlay_subsurface);
    {
        wl_region* empty = wl_compositor_create_region(g_wl.compositor);
        wl_surface_set_input_region(g_wl.overlay_surface, empty);
        wl_region_destroy(empty);
    }
    if (g_wl.viewporter)
        g_wl.overlay_viewport = wp_viewporter_get_viewport(g_wl.viewporter, g_wl.overlay_surface);
    if (g_wl.alpha_modifier)
        g_wl.overlay_alpha = wp_alpha_modifier_v1_get_surface(g_wl.alpha_modifier, g_wl.overlay_surface);
    wl_surface_commit(g_wl.overlay_surface);

    // --- Popup subsurface (child of cef_surface, for CEF OSR <select> dropdowns) ---
    // Must be a child of cef_surface (not the parent) so that
    // wl_subsurface_set_position takes effect on cef_surface's commit
    // rather than waiting for mpv's parent commit, which we don't control.
    g_wl.popup_surface = wl_compositor_create_surface(g_wl.compositor);
    g_wl.popup_subsurface = wl_subcompositor_get_subsurface(g_wl.subcompositor, g_wl.popup_surface, g_wl.cef_surface);
    wl_subsurface_set_desync(g_wl.popup_subsurface);
    {
        wl_region* empty = wl_compositor_create_region(g_wl.compositor);
        wl_surface_set_input_region(g_wl.popup_surface, empty);
        wl_region_destroy(empty);
    }
    if (g_wl.viewporter)
        g_wl.popup_viewport = wp_viewporter_get_viewport(g_wl.viewporter, g_wl.popup_surface);
    wl_surface_commit(g_wl.popup_surface);

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
        LOG_INFO(LOG_PLATFORM, "Shared textures not supported; using software CEF rendering");
        g_platform.shared_texture_supported = false;
    }

    // KDE titlebar color — matches the loading screen background
    wl_init_kde_palette();
    wl_set_titlebar_color(kBgColor.r, kBgColor.g, kBgColor.b);

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
    if (!g_mpv.IsValid()) return 1.0f;
    double scale = 0;
    if (g_mpv.GetDisplayScale(scale) >= 0 && scale > 0) {
        g_wl.cached_scale = static_cast<float>(scale);
        return g_wl.cached_scale;
    }
    return g_wl.cached_scale > 0 ? g_wl.cached_scale : 1.0f;
}

static void wl_cleanup() {
    wl_cleanup_kde_palette();
    idle_inhibit::cleanup();
    // Clipboard worker owns its own thread + wl_event_queue; must shut
    // down before input::wayland::cleanup destroys the seat it borrowed.
    clipboard_wayland::cleanup();
    // Input layer owns seat/pointer/keyboard/xkb/cursor-shape-device.
    input::wayland::cleanup();
    // Popup
    if (g_wl.popup_viewport) wp_viewport_destroy(g_wl.popup_viewport);
    if (g_wl.popup_buffer) wl_buffer_destroy(g_wl.popup_buffer);
    if (g_wl.popup_subsurface) wl_subsurface_destroy(g_wl.popup_subsurface);
    if (g_wl.popup_surface) wl_surface_destroy(g_wl.popup_surface);
    // Overlay
    if (g_wl.overlay_alpha) { wp_alpha_modifier_surface_v1_destroy(g_wl.overlay_alpha); g_wl.overlay_alpha = nullptr; }
    if (g_wl.overlay_viewport) wp_viewport_destroy(g_wl.overlay_viewport);
    if (g_wl.overlay_buffer) wl_buffer_destroy(g_wl.overlay_buffer);
    if (g_wl.overlay_subsurface) wl_subsurface_destroy(g_wl.overlay_subsurface);
    if (g_wl.overlay_surface) wl_surface_destroy(g_wl.overlay_surface);
    // Main
    if (g_wl.cef_viewport) wp_viewport_destroy(g_wl.cef_viewport);
    if (g_wl.cef_buffer) wl_buffer_destroy(g_wl.cef_buffer);
    if (g_wl.cef_subsurface) wl_subsurface_destroy(g_wl.cef_subsurface);
    if (g_wl.cef_surface) wl_surface_destroy(g_wl.cef_surface);
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

// Update main subsurface viewport. Caller must hold surface_mtx.
static void update_surface_size_locked(int lw, int lh, int pw, int ph) {
    if (g_wl.transitioning) {
        g_wl.pending_lw = lw;
        g_wl.pending_lh = lh;
        if (g_wl.cef_surface && g_wl.cef_viewport) {
            wp_viewport_set_destination(g_wl.cef_viewport, lw, lh);
            wl_surface_commit(g_wl.cef_surface);
            wl_display_flush(g_wl.display);
        }
    } else if (g_wl.cef_surface) {
        bool growing = pw > g_wl.mpv_pw || ph > g_wl.mpv_ph;
        if (growing)
            wl_surface_attach(g_wl.cef_surface, nullptr, 0, 0);
        if (g_wl.cef_viewport) {
            wp_viewport_set_source(g_wl.cef_viewport,
                wl_fixed_from_int(0), wl_fixed_from_int(0),
                wl_fixed_from_int(pw), wl_fixed_from_int(ph));
            wp_viewport_set_destination(g_wl.cef_viewport, lw, lh);
        }
        wl_surface_commit(g_wl.cef_surface);
        wl_display_flush(g_wl.display);
    }
    g_wl.mpv_pw = pw;
    g_wl.mpv_ph = ph;
}

static void wl_resize(int lw, int lh, int pw, int ph) {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    update_surface_size_locked(lw, lh, pw, ph);
}

static void wl_begin_transition_locked() {
    g_wl.transitioning = true;
    g_wl.transition_pw = g_wl.mpv_pw;
    g_wl.transition_ph = g_wl.mpv_ph;
    g_wl.pending_lw = 0;
    g_wl.pending_lh = 0;
    if (g_wl.cef_surface) {
        wl_surface_attach(g_wl.cef_surface, nullptr, 0, 0);
        if (g_wl.cef_viewport)
            wp_viewport_set_destination(g_wl.cef_viewport, -1, -1);
        wl_surface_commit(g_wl.cef_surface);
        wl_display_flush(g_wl.display);
    }
}

static void wl_end_transition_locked() {
    g_wl.transitioning = false;
    g_wl.expected_w = 0;
    g_wl.expected_h = 0;
    if (g_wl.cef_viewport && g_wl.pending_lw > 0) {
        wp_viewport_set_source(g_wl.cef_viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(g_wl.mpv_pw), wl_fixed_from_int(g_wl.mpv_ph));
        wp_viewport_set_destination(g_wl.cef_viewport, g_wl.pending_lw, g_wl.pending_lh);
        g_wl.pending_lw = 0;
        g_wl.pending_lh = 0;
    }
}

static void wl_set_fullscreen(bool fullscreen) {
    if (!g_mpv.IsValid()) return;
    // Only transition if state actually changes
    // Safe to call from CEF thread: this is cached in mpv's option struct,
    // not a VO property — no VO lock contention.
    bool current = false;
    if (g_mpv.GetFullscreen(current) >= 0) {
        if (current == fullscreen) {
            // Compositor may have rejected our fullscreen change.
            // If we're mid-transition and the state matches the pre-lock
            // value (was_fullscreen), the compositor forced us back — cancel.
            std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
            if (g_wl.transitioning && fullscreen == g_wl.was_fullscreen)
                wl_end_transition_locked();
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
        wl_begin_transition_locked();
    }
    g_mpv.SetFullscreen(fullscreen);
}

static void wl_toggle_fullscreen() {
    {
        std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
        wl_begin_transition_locked();
    }
    if (g_mpv.IsValid()) {
        g_mpv.ToggleFullscreen();
    }
}

static void wl_begin_transition() {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    wl_begin_transition_locked();
}

static bool wl_in_transition() {
    return g_wl.transitioning;
}

static void wl_set_expected_size_locked(int w, int h) {
    if (g_wl.transitioning && w == g_wl.transition_pw && h == g_wl.transition_ph)
        return;
    g_wl.expected_w = w;
    g_wl.expected_h = h;
}

static void wl_set_expected_size(int w, int h) {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    wl_set_expected_size_locked(w, h);
}

static void wl_end_transition() {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    wl_end_transition_locked();
}

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

static bool writeColorScheme(uint8_t r, uint8_t g, uint8_t b, const std::string& path) {
    char bg[32];
    snprintf(bg, sizeof(bg), "%d,%d,%d", r, g, b);

    // BT.709 luminance — choose readable foreground
    double lum = 0.2126 * (r / 255.0) + 0.7152 * (g / 255.0) + 0.0722 * (b / 255.0);
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
    if (g_wl.palette) {
        org_kde_kwin_server_decoration_palette_release(g_wl.palette);
        g_wl.palette = nullptr;
    }
    if (g_wl.palette_manager) {
        org_kde_kwin_server_decoration_palette_manager_destroy(g_wl.palette_manager);
        g_wl.palette_manager = nullptr;
    }
    if (!g_wl.colors_path.empty()) {
        remove(g_wl.colors_path.c_str());
        g_wl.colors_path.clear();
    }
}

static void wl_set_titlebar_color(uint8_t r, uint8_t g, uint8_t b) {
    LOG_DEBUG(LOG_PLATFORM, "set_titlebar_color({:02x},{:02x},{:02x}) palette={}", r, g, b, (void*)g_wl.palette);
    if (!g_wl.palette) return;

    char filename[64];
    snprintf(filename, sizeof(filename), "JellyfinDesktop-%02x%02x%02x.colors", r, g, b);
    std::string new_path = g_wl.colors_dir + "/" + filename;
    if (new_path == g_wl.colors_path) return;

    if (!writeColorScheme(r, g, b, new_path)) return;

    if (!g_wl.colors_path.empty())
        remove(g_wl.colors_path.c_str());
    g_wl.colors_path = new_path;

    org_kde_kwin_server_decoration_palette_set_palette(g_wl.palette, g_wl.colors_path.c_str());
}

#else
static void wl_init_kde_palette() {}
static void wl_cleanup_kde_palette() {}
static void wl_set_titlebar_color(uint8_t, uint8_t, uint8_t) {}
#endif

Platform make_wayland_platform() {
    return Platform{
        .display = DisplayBackend::Wayland,
        .early_init = []() {},
        .init = wl_init,
        .cleanup = wl_cleanup,
        .present = wl_present,
        .present_software = wl_present_software,
        .resize = wl_resize,
        .overlay_present = wl_overlay_present,
        .overlay_present_software = wl_overlay_present_software,
        .overlay_resize = wl_overlay_resize,
        .set_overlay_visible = wl_set_overlay_visible,
        .popup_show = wl_popup_show,
        .popup_hide = wl_popup_hide,
        .popup_present = wl_popup_present,
        .popup_present_software = wl_popup_present_software,
        .try_native_popup_menu = [](int, int, int, int,
                                    const std::vector<std::string>&, int,
                                    std::function<void(int)>) { return false; },
        .fade_overlay = wl_fade_overlay,
        .set_fullscreen = wl_set_fullscreen,
        .toggle_fullscreen = wl_toggle_fullscreen,
        .begin_transition = wl_begin_transition,
        .end_transition = wl_end_transition,
        .in_transition = wl_in_transition,
        .set_expected_size = wl_set_expected_size,
        .get_scale = wl_get_scale,
        .query_window_position = [](int*, int*) -> bool { return false; },
        .clamp_window_geometry = nullptr,
        .pump = wl_pump,
        .set_cursor = input::wayland::set_cursor,
        .set_idle_inhibit = wl_set_idle_inhibit,
        .set_titlebar_color = wl_set_titlebar_color,
        .clipboard_read_text_async = clipboard_wayland::read_text_async,
        .open_external_url = open_url_linux::open,
    };
}
