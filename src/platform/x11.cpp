#include "common.h"
#include "cef/cef_client.h"
#include "browser/browsers.h"
#include "browser/web_browser.h"
#include "browser/overlay_browser.h"
#include "idle_inhibit_linux.h"
#include "open_url_linux.h"
#include "input/input_x11.h"
#include "mpv/event.h"

#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/shape.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <mutex>
#include <thread>
#include <sys/shm.h>
#include "logging.h"


// =====================================================================
// X11 state (file-static)
// =====================================================================

struct ShmBuffer {
    xcb_shm_seg_t seg = 0;
    int shmid = -1;
    uint8_t* data = nullptr;
    int w = 0, h = 0;
    size_t size = 0;
};

struct X11State {
    std::mutex surface_mtx;
    xcb_connection_t* conn = nullptr;
    int screen_num = 0;
    xcb_screen_t* screen = nullptr;

    xcb_window_t parent = XCB_NONE;  // mpv's window

    // Main browser child window
    xcb_window_t cef_window = XCB_NONE;
    ShmBuffer cef_bufs[2];
    int cef_buf_idx = 0;

    // Overlay browser child window
    xcb_window_t overlay_window = XCB_NONE;
    ShmBuffer overlay_bufs[2];
    int overlay_buf_idx = 0;
    bool overlay_visible = false;

    // About browser child window (above overlay)
    xcb_window_t about_window = XCB_NONE;
    ShmBuffer about_bufs[2];
    int about_buf_idx = 0;
    bool about_visible = false;

    // Graphics contexts (one per child window, reused across frames)
    xcb_gcontext_t cef_gc = XCB_NONE;
    xcb_gcontext_t overlay_gc = XCB_NONE;
    xcb_gcontext_t about_gc = XCB_NONE;

    // ARGB visual
    xcb_visualid_t argb_visual = 0;
    uint8_t argb_depth = 0;
    xcb_colormap_t colormap = XCB_NONE;

    // Dimensions
    float cached_scale = 1.0f;
    int pw = 0, ph = 0;

    // Fade
    bool transitioning = false;
    int transition_pw = 0, transition_ph = 0;
    int pending_lw = 0, pending_lh = 0;
    int expected_w = 0, expected_h = 0;
    bool was_fullscreen = false;

    // Atoms
    xcb_atom_t net_wm_opacity = XCB_NONE;
    xcb_atom_t net_wm_window_type = XCB_NONE;
    xcb_atom_t net_wm_window_type_notification = XCB_NONE;
    xcb_atom_t net_wm_state = XCB_NONE;
    xcb_atom_t net_wm_state_above = XCB_NONE;
    xcb_atom_t net_wm_state_skip_taskbar = XCB_NONE;
    xcb_atom_t net_wm_state_skip_pager = XCB_NONE;
    xcb_atom_t wm_protocols = XCB_NONE;
    xcb_atom_t wm_delete_window = XCB_NONE;

    // Parent position (for overlay positioning)
    int parent_x = 0, parent_y = 0;
};

static X11State g_x11;

// =====================================================================
// Helpers
// =====================================================================

static xcb_visualid_t find_argb_visual(xcb_screen_t* screen, uint8_t* depth_out) {
    for (auto depth_iter = xcb_screen_allowed_depths_iterator(screen);
         depth_iter.rem; xcb_depth_next(&depth_iter)) {
        if (depth_iter.data->depth != 32) continue;
        for (auto vis_iter = xcb_depth_visuals_iterator(depth_iter.data);
             vis_iter.rem; xcb_visualtype_next(&vis_iter)) {
            if (vis_iter.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
                *depth_out = 32;
                return vis_iter.data->visual_id;
            }
        }
    }
    return 0;
}

static xcb_atom_t intern_atom(xcb_connection_t* conn, const char* name) {
    auto cookie = xcb_intern_atom(conn, 0, strlen(name), name);
    auto* reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    if (!reply) return XCB_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

// Query parent window's absolute position on screen.
static bool query_parent_geometry(int* x, int* y, int* w, int* h) {
    auto geo_cookie = xcb_get_geometry(g_x11.conn, g_x11.parent);
    auto* geo = xcb_get_geometry_reply(g_x11.conn, geo_cookie, nullptr);
    if (!geo) return false;
    if (w) *w = geo->width;
    if (h) *h = geo->height;
    free(geo);

    auto trans_cookie = xcb_translate_coordinates(g_x11.conn,
        g_x11.parent, g_x11.screen->root, 0, 0);
    auto* trans = xcb_translate_coordinates_reply(g_x11.conn, trans_cookie, nullptr);
    if (!trans) return false;
    if (x) *x = trans->dst_x;
    if (y) *y = trans->dst_y;
    free(trans);
    return true;
}

// Reposition overlay windows to match mpv's parent window.
static void sync_overlay_positions() {
    int px, py, pw, ph;
    if (!query_parent_geometry(&px, &py, &pw, &ph)) return;

    uint32_t vals[4] = {
        static_cast<uint32_t>(px), static_cast<uint32_t>(py),
        static_cast<uint32_t>(pw), static_cast<uint32_t>(ph)
    };
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;

    if (g_x11.cef_window != XCB_NONE)
        xcb_configure_window(g_x11.conn, g_x11.cef_window, mask, vals);
    if (g_x11.overlay_window != XCB_NONE && g_x11.overlay_visible)
        xcb_configure_window(g_x11.conn, g_x11.overlay_window, mask, vals);
    if (g_x11.about_window != XCB_NONE && g_x11.about_visible)
        xcb_configure_window(g_x11.conn, g_x11.about_window, mask, vals);
    xcb_flush(g_x11.conn);
}

// =====================================================================
// SHM buffer management
// =====================================================================

static bool shm_alloc(ShmBuffer& buf, xcb_connection_t* conn, int w, int h) {
    size_t size = static_cast<size_t>(w) * h * 4;
    if (buf.data && buf.w == w && buf.h == h) return true;

    // Free old
    if (buf.data) {
        xcb_shm_detach(conn, buf.seg);
        shmdt(buf.data);
        shmctl(buf.shmid, IPC_RMID, nullptr);
        buf.data = nullptr;
    }

    buf.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
    if (buf.shmid < 0) return false;

    buf.data = static_cast<uint8_t*>(shmat(buf.shmid, nullptr, 0));
    if (buf.data == reinterpret_cast<uint8_t*>(-1)) {
        shmctl(buf.shmid, IPC_RMID, nullptr);
        buf.data = nullptr;
        return false;
    }
    // Mark for removal — will be freed when last process detaches
    shmctl(buf.shmid, IPC_RMID, nullptr);

    buf.seg = xcb_generate_id(conn);
    xcb_shm_attach(conn, buf.seg, buf.shmid, 0);

    buf.w = w;
    buf.h = h;
    buf.size = size;
    return true;
}

static void shm_free(ShmBuffer& buf, xcb_connection_t* conn) {
    if (!buf.data) return;
    xcb_shm_detach(conn, buf.seg);
    shmdt(buf.data);
    buf.data = nullptr;
    buf.w = buf.h = 0;
    buf.size = 0;
}

// =====================================================================
// Present CEF software -- main browser
// =====================================================================

static void hide_overlays_locked() {
    if (g_x11.about_window != XCB_NONE)
        xcb_unmap_window(g_x11.conn, g_x11.about_window);
    if (g_x11.overlay_window != XCB_NONE)
        xcb_unmap_window(g_x11.conn, g_x11.overlay_window);
    if (g_x11.cef_window != XCB_NONE)
        xcb_unmap_window(g_x11.conn, g_x11.cef_window);
    xcb_flush(g_x11.conn);
}

static void x11_present_software(const CefRenderHandler::RectList& dirty,
                                 const void* buffer, int w, int h) {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
    if (g_x11.cef_window == XCB_NONE) return;

    auto& buf = g_x11.cef_bufs[g_x11.cef_buf_idx];
    if (!shm_alloc(buf, g_x11.conn, w, h)) return;

    int stride = w * 4;
    const auto* src = static_cast<const uint8_t*>(buffer);

    for (const auto& rect : dirty) {
        int rx = rect.x, ry = rect.y, rw = rect.width, rh = rect.height;
        // Clamp to buffer
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > w) rw = w - rx;
        if (ry + rh > h) rh = h - ry;
        if (rw <= 0 || rh <= 0) continue;

        // Copy dirty region into shm buffer
        for (int row = ry; row < ry + rh; row++) {
            memcpy(buf.data + row * stride + rx * 4,
                   src + row * stride + rx * 4,
                   rw * 4);
        }

        xcb_shm_put_image(g_x11.conn, g_x11.cef_window, g_x11.cef_gc,
            w, h, rx, ry, rw, rh,
            rx, ry, g_x11.argb_depth,
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            0, buf.seg, 0);
    }

    g_x11.cef_buf_idx ^= 1;
    xcb_flush(g_x11.conn);
}

// =====================================================================
// Present CEF software -- overlay browser
// =====================================================================

static void x11_overlay_present_software(const CefRenderHandler::RectList& dirty,
                                         const void* buffer, int w, int h) {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
    if (g_x11.overlay_window == XCB_NONE || !g_x11.overlay_visible) return;

    auto& buf = g_x11.overlay_bufs[g_x11.overlay_buf_idx];
    if (!shm_alloc(buf, g_x11.conn, w, h)) return;

    int stride = w * 4;
    const auto* src = static_cast<const uint8_t*>(buffer);

    for (const auto& rect : dirty) {
        int rx = rect.x, ry = rect.y, rw = rect.width, rh = rect.height;
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > w) rw = w - rx;
        if (ry + rh > h) rh = h - ry;
        if (rw <= 0 || rh <= 0) continue;

        for (int row = ry; row < ry + rh; row++) {
            memcpy(buf.data + row * stride + rx * 4,
                   src + row * stride + rx * 4,
                   rw * 4);
        }

        xcb_shm_put_image(g_x11.conn, g_x11.overlay_window, g_x11.overlay_gc,
            w, h, rx, ry, rw, rh,
            rx, ry, g_x11.argb_depth,
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            0, buf.seg, 0);
    }

    g_x11.overlay_buf_idx ^= 1;
    xcb_flush(g_x11.conn);
}

// =====================================================================
// Resize
// =====================================================================

static void x11_resize(int lw, int lh, int pw, int ph) {
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
    g_x11.pw = pw;
    g_x11.ph = ph;

    // Overlays are top-level — reposition to match mpv's window
    sync_overlay_positions();
}

static void x11_overlay_resize(int, int, int, int) {
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
    sync_overlay_positions();
}

// =====================================================================
// Present CEF software -- about browser
// =====================================================================

static void x11_about_present_software(const CefRenderHandler::RectList& dirty,
                                       const void* buffer, int w, int h) {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
    if (g_x11.about_window == XCB_NONE || !g_x11.about_visible) return;

    auto& buf = g_x11.about_bufs[g_x11.about_buf_idx];
    if (!shm_alloc(buf, g_x11.conn, w, h)) return;

    int stride = w * 4;
    const auto* src = static_cast<const uint8_t*>(buffer);

    for (const auto& rect : dirty) {
        int rx = rect.x, ry = rect.y, rw = rect.width, rh = rect.height;
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > w) rw = w - rx;
        if (ry + rh > h) rh = h - ry;
        if (rw <= 0 || rh <= 0) continue;

        for (int row = ry; row < ry + rh; row++) {
            memcpy(buf.data + row * stride + rx * 4,
                   src + row * stride + rx * 4,
                   rw * 4);
        }

        xcb_shm_put_image(g_x11.conn, g_x11.about_window, g_x11.about_gc,
            w, h, rx, ry, rw, rh,
            rx, ry, g_x11.argb_depth,
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            0, buf.seg, 0);
    }

    g_x11.about_buf_idx ^= 1;
    xcb_flush(g_x11.conn);
}

static void x11_about_resize(int, int, int, int) {
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
    sync_overlay_positions();
}

static void x11_set_about_visible(bool visible) {
    {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        if (g_x11.about_visible == visible) return;
        g_x11.about_visible = visible;
        if (g_x11.about_window == XCB_NONE) return;

        if (visible) {
            if (g_x11.pw > 0 && g_x11.ph > 0) {
                uint32_t vals[2] = {static_cast<uint32_t>(g_x11.pw),
                                    static_cast<uint32_t>(g_x11.ph)};
                xcb_configure_window(g_x11.conn, g_x11.about_window,
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
            }
            xcb_map_window(g_x11.conn, g_x11.about_window);
            xcb_flush(g_x11.conn);
        } else {
            xcb_unmap_window(g_x11.conn, g_x11.about_window);
            xcb_flush(g_x11.conn);
        }
    }

    if (visible) {
        auto main = g_web_browser ? g_web_browser->browser() : nullptr;
        auto ovl  = g_overlay_browser ? g_overlay_browser->browser() : nullptr;
        if (main) main->GetHost()->SetFocus(false);
        if (ovl)  ovl->GetHost()->SetFocus(false);
    }
}

// =====================================================================
// Overlay visibility
// =====================================================================

static void x11_set_overlay_visible(bool visible) {
    {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        if (g_x11.overlay_visible == visible) return;
        g_x11.overlay_visible = visible;
        if (g_x11.overlay_window == XCB_NONE) return;

        if (visible) {
            // Resize to current window size and map
            if (g_x11.pw > 0 && g_x11.ph > 0) {
                uint32_t vals[2] = {static_cast<uint32_t>(g_x11.pw),
                                    static_cast<uint32_t>(g_x11.ph)};
                xcb_configure_window(g_x11.conn, g_x11.overlay_window,
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
            }
            xcb_map_window(g_x11.conn, g_x11.overlay_window);
            xcb_flush(g_x11.conn);
        } else {
            // Reset opacity to fully opaque for next time
            if (g_x11.net_wm_opacity != XCB_NONE) {
                uint32_t opacity = 0xFFFFFFFF;
                xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE,
                    g_x11.overlay_window, g_x11.net_wm_opacity,
                    XCB_ATOM_CARDINAL, 32, 1, &opacity);
            }
            xcb_unmap_window(g_x11.conn, g_x11.overlay_window);
            xcb_flush(g_x11.conn);
        }
    }

    // Route keyboard focus to the active browser
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

// =====================================================================
// Fade overlay
// =====================================================================

static void x11_fade_overlay(float fade_sec,
                             std::function<void()> on_fade_start,
                             std::function<void()> on_complete) {
    if (g_x11.net_wm_opacity == XCB_NONE) {
        // No opacity support — just hide immediately
        x11_set_overlay_visible(false);
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
            uint32_t alpha = static_cast<uint32_t>((1.0f - t) * 0xFFFFFFFF);

            {
                std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
                if (!g_x11.overlay_visible || g_x11.overlay_window == XCB_NONE) break;
                xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE,
                    g_x11.overlay_window, g_x11.net_wm_opacity,
                    XCB_ATOM_CARDINAL, 32, 1, &alpha);
                xcb_flush(g_x11.conn);
            }
            std::this_thread::sleep_for(frame_duration);
        }

        x11_set_overlay_visible(false);
        if (on_complete) on_complete();
    }).detach();
}

// =====================================================================
// Fullscreen
// =====================================================================

static void x11_set_fullscreen(bool fullscreen) {
    if (!g_mpv.IsValid()) return;
    if (mpv::fullscreen() == fullscreen) return;
    g_mpv.SetFullscreen(fullscreen);
}

static void x11_toggle_fullscreen() {
    if (g_mpv.IsValid()) g_mpv.ToggleFullscreen();
}

// =====================================================================
// Transition stubs (X11 doesn't need Wayland-style transition gating)
// =====================================================================

static void x11_begin_transition() {}
static void x11_end_transition() {}
static bool x11_in_transition() { return false; }
static void x11_set_expected_size(int, int) {}

// =====================================================================
// Scale
// =====================================================================

static float x11_get_scale() {
    double scale = mpv::display_scale();
    if (scale > 0) {
        g_x11.cached_scale = static_cast<float>(scale);
        return g_x11.cached_scale;
    }
    return g_x11.cached_scale > 0 ? g_x11.cached_scale : 1.0f;
}

// =====================================================================
// Init
// =====================================================================

static bool x11_init(mpv_handle*) {
    // Get mpv's window ID
    int64_t wid = 0;
    g_mpv.GetWindowId(wid);
    if (wid <= 0) {
        fprintf(stderr, "Failed to get window-id from mpv\n");
        return false;
    }
    g_x11.parent = static_cast<xcb_window_t>(wid);

    // Open XCB connection
    g_x11.conn = xcb_connect(nullptr, &g_x11.screen_num);
    if (xcb_connection_has_error(g_x11.conn)) {
        fprintf(stderr, "Failed to connect to X11\n");
        return false;
    }

    // Find screen
    auto setup = xcb_get_setup(g_x11.conn);
    auto screen_iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < g_x11.screen_num; i++)
        xcb_screen_next(&screen_iter);
    g_x11.screen = screen_iter.data;

    // Find 32-bit ARGB visual
    g_x11.argb_visual = find_argb_visual(g_x11.screen, &g_x11.argb_depth);
    if (!g_x11.argb_visual) {
        fprintf(stderr, "No 32-bit ARGB visual found\n");
        xcb_disconnect(g_x11.conn);
        return false;
    }

    // Create colormap for ARGB visual
    g_x11.colormap = xcb_generate_id(g_x11.conn);
    xcb_create_colormap(g_x11.conn, XCB_COLORMAP_ALLOC_NONE,
        g_x11.colormap, g_x11.screen->root, g_x11.argb_visual);

    // Intern atoms
    g_x11.net_wm_opacity = intern_atom(g_x11.conn, "_NET_WM_WINDOW_OPACITY");
    g_x11.net_wm_window_type = intern_atom(g_x11.conn, "_NET_WM_WINDOW_TYPE");
    g_x11.net_wm_window_type_notification = intern_atom(g_x11.conn, "_NET_WM_WINDOW_TYPE_NOTIFICATION");
    g_x11.net_wm_state = intern_atom(g_x11.conn, "_NET_WM_STATE");
    g_x11.net_wm_state_above = intern_atom(g_x11.conn, "_NET_WM_STATE_ABOVE");
    g_x11.net_wm_state_skip_taskbar = intern_atom(g_x11.conn, "_NET_WM_STATE_SKIP_TASKBAR");
    g_x11.net_wm_state_skip_pager = intern_atom(g_x11.conn, "_NET_WM_STATE_SKIP_PAGER");
    g_x11.wm_protocols = intern_atom(g_x11.conn, "WM_PROTOCOLS");
    g_x11.wm_delete_window = intern_atom(g_x11.conn, "WM_DELETE_WINDOW");

    // Check for SHM extension
    auto shm_cookie = xcb_shm_query_version(g_x11.conn);
    auto* shm_reply = xcb_shm_query_version_reply(g_x11.conn, shm_cookie, nullptr);
    if (!shm_reply) {
        fprintf(stderr, "X11 MIT-SHM extension not available\n");
        xcb_free_colormap(g_x11.conn, g_x11.colormap);
        xcb_disconnect(g_x11.conn);
        return false;
    }
    free(shm_reply);

    // Get mpv window geometry for initial overlay positioning
    int px = 0, py = 0, pw = 1, ph = 1;
    query_parent_geometry(&px, &py, &pw, &ph);
    g_x11.parent_x = px;
    g_x11.parent_y = py;

    // Helper: create a top-level ARGB overlay window.
    // Override-redirect: no WM decoration, no management.
    // Compositor alpha-blends against windows behind (mpv).
    auto create_overlay_window = [&](int x, int y, int w, int h) -> xcb_window_t {
        xcb_window_t win = xcb_generate_id(g_x11.conn);
        uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_COLORMAP;
        uint32_t vals[4] = {0, 0, 1, g_x11.colormap};
        xcb_create_window(g_x11.conn, g_x11.argb_depth,
            win, g_x11.screen->root,
            x, y, w, h, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            g_x11.argb_visual, mask, vals);

        // Input-passthrough: empty input shape
        xcb_shape_rectangles(g_x11.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
            XCB_CLIP_ORDERING_UNSORTED, win, 0, 0, 0, nullptr);

        // Handle WM_DELETE_WINDOW if the WM ever targets this window
        xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, win,
            g_x11.wm_protocols, XCB_ATOM_ATOM, 32, 1,
            &g_x11.wm_delete_window);

        return win;
    };

    // Main CEF overlay (always mapped, alpha-transparent over mpv)
    g_x11.cef_window = create_overlay_window(px, py, pw, ph);
    g_x11.cef_gc = xcb_generate_id(g_x11.conn);
    xcb_create_gc(g_x11.conn, g_x11.cef_gc, g_x11.cef_window, 0, nullptr);
    xcb_map_window(g_x11.conn, g_x11.cef_window);

    // Overlay CEF window (above main, initially unmapped)
    g_x11.overlay_window = create_overlay_window(px, py, pw, ph);
    g_x11.overlay_gc = xcb_generate_id(g_x11.conn);
    xcb_create_gc(g_x11.conn, g_x11.overlay_gc, g_x11.overlay_window, 0, nullptr);

    // About CEF window (above overlay, initially unmapped)
    g_x11.about_window = create_overlay_window(px, py, pw, ph);
    g_x11.about_gc = xcb_generate_id(g_x11.conn);
    xcb_create_gc(g_x11.conn, g_x11.about_gc, g_x11.about_window, 0, nullptr);

    xcb_flush(g_x11.conn);

    // Note: input::x11::init already selects StructureNotify + input events
    // on the parent window, so ConfigureNotify is delivered to the input thread.

    // Software rendering only for now
    g_platform.shared_texture_supported = false;

    // Init input on mpv's parent window
    input::x11::init(g_x11.conn, g_x11.screen, g_x11.parent);
    input::x11::set_configure_callback([]() { sync_overlay_positions(); });
    input::x11::set_shutdown_callback([]() {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        hide_overlays_locked();
    });
    input::x11::start_input_thread();

    idle_inhibit::init();

    LOG_INFO(LOG_PLATFORM, "X11 platform initialized (parent=0x{:x})", g_x11.parent);
    return true;
}

// =====================================================================
// Cleanup
// =====================================================================

static void x11_cleanup() {
    // Hide overlay windows immediately so they don't linger during shutdown
    if (g_x11.conn) {
        if (g_x11.about_window != XCB_NONE)
            xcb_unmap_window(g_x11.conn, g_x11.about_window);
        if (g_x11.overlay_window != XCB_NONE)
            xcb_unmap_window(g_x11.conn, g_x11.overlay_window);
        if (g_x11.cef_window != XCB_NONE)
            xcb_unmap_window(g_x11.conn, g_x11.cef_window);
        xcb_flush(g_x11.conn);
    }

    idle_inhibit::cleanup();
    input::x11::cleanup();

    // Free SHM buffers
    for (auto& buf : g_x11.cef_bufs)     shm_free(buf, g_x11.conn);
    for (auto& buf : g_x11.overlay_bufs)  shm_free(buf, g_x11.conn);
    for (auto& buf : g_x11.about_bufs)    shm_free(buf, g_x11.conn);

    // Free GCs and destroy windows
    if (g_x11.about_gc != XCB_NONE)
        xcb_free_gc(g_x11.conn, g_x11.about_gc);
    if (g_x11.about_window != XCB_NONE)
        xcb_destroy_window(g_x11.conn, g_x11.about_window);
    if (g_x11.overlay_gc != XCB_NONE)
        xcb_free_gc(g_x11.conn, g_x11.overlay_gc);
    if (g_x11.cef_gc != XCB_NONE)
        xcb_free_gc(g_x11.conn, g_x11.cef_gc);
    if (g_x11.overlay_window != XCB_NONE)
        xcb_destroy_window(g_x11.conn, g_x11.overlay_window);
    if (g_x11.cef_window != XCB_NONE)
        xcb_destroy_window(g_x11.conn, g_x11.cef_window);
    if (g_x11.colormap != XCB_NONE)
        xcb_free_colormap(g_x11.conn, g_x11.colormap);

    if (g_x11.conn) {
        xcb_disconnect(g_x11.conn);
        g_x11.conn = nullptr;
    }
}

// =====================================================================
// Platform factory
// =====================================================================

Platform make_x11_platform() {
    return Platform{
        .display = DisplayBackend::X11,
        .early_init = []() {},
        .init = x11_init,
        .cleanup = x11_cleanup,
        .present = [](const CefAcceleratedPaintInfo&) {},
        .present_software = x11_present_software,
        .resize = x11_resize,
        .overlay_present = [](const CefAcceleratedPaintInfo&) {},
        .overlay_present_software = x11_overlay_present_software,
        .overlay_resize = x11_overlay_resize,
        .set_overlay_visible = x11_set_overlay_visible,
        .about_present = [](const CefAcceleratedPaintInfo&) {},
        .about_present_software = x11_about_present_software,
        .about_resize = x11_about_resize,
        .set_about_visible = x11_set_about_visible,
        .popup_show = [](int, int, int, int) {},
        .popup_hide = []() {},
        .popup_present = [](const CefAcceleratedPaintInfo&, int, int) {},
        .popup_present_software = [](const void*, int, int, int, int) {},
        .try_native_popup_menu = [](int, int, int, int,
                                    const std::vector<std::string>&, int,
                                    std::function<void(int)>) { return false; },
        .fade_overlay = x11_fade_overlay,
        .set_fullscreen = x11_set_fullscreen,
        .toggle_fullscreen = x11_toggle_fullscreen,
        .begin_transition = x11_begin_transition,
        .end_transition = x11_end_transition,
        .in_transition = x11_in_transition,
        .set_expected_size = x11_set_expected_size,
        .get_scale = x11_get_scale,
        .query_window_position = [](int* x, int* y) -> bool {
            if (!g_x11.conn || g_x11.parent == XCB_NONE) return false;
            auto cookie = xcb_translate_coordinates(g_x11.conn,
                g_x11.parent, g_x11.screen->root, 0, 0);
            auto* reply = xcb_translate_coordinates_reply(g_x11.conn, cookie, nullptr);
            if (!reply) return false;
            *x = reply->dst_x;
            *y = reply->dst_y;
            free(reply);
            return true;
        },
        .clamp_window_geometry = nullptr,
        .pump = []() {},
        .run_main_loop = nullptr,
        .wake_main_loop = nullptr,
        .set_cursor = input::x11::set_cursor,
        .set_idle_inhibit = [](IdleInhibitLevel level) { idle_inhibit::set(level); },
        .set_titlebar_color = [](uint8_t, uint8_t, uint8_t) {},
        .shared_texture_supported = false,
        .clipboard_read_text_async = nullptr,
        .open_external_url = open_url_linux::open,
    };
}
