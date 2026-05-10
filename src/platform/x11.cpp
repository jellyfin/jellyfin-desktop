#include "common.h"
#include "cef/cef_client.h"
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
#include <algorithm>
#include <mutex>
#include <vector>
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

// Per-surface state. Each PlatformSurface is a top-level ARGB
// override-redirect window positioned over mpv's window. The compositor
// alpha-blends transparent regions against the video behind. X11 backend
// is software-only — no GL / shared-texture path.
struct PlatformSurface {
    xcb_window_t window = XCB_NONE;
    xcb_gcontext_t gc = XCB_NONE;
    ShmBuffer bufs[2];
    int buf_idx = 0;
    bool visible = true;   // mapped by default at alloc, matches Wayland
    int pw = 0, ph = 0;    // physical size last applied via surface_resize
};

struct X11State {
    std::mutex surface_mtx;
    xcb_connection_t* conn = nullptr;
    int screen_num = 0;
    xcb_screen_t* screen = nullptr;

    xcb_window_t parent = XCB_NONE;  // mpv's window

    // ARGB visual
    xcb_visualid_t argb_visual = 0;
    uint8_t argb_depth = 0;
    xcb_colormap_t colormap = XCB_NONE;

    // Dimensions tracked from latest surface_resize (used to size newly
    // created surfaces before any resize lands)
    float cached_scale = 1.0f;
    int pw = 0, ph = 0;

    // Live surfaces — used by sync_overlay_positions on ConfigureNotify
    // and by cleanup. Mutated only under surface_mtx.
    std::vector<PlatformSurface*> live;

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

// Reposition every live surface to match mpv's parent window. Called from
// the input thread on ConfigureNotify. surface_mtx held by caller.
static void sync_overlay_positions_locked() {
    int px, py, pw, ph;
    if (!query_parent_geometry(&px, &py, &pw, &ph)) return;

    g_x11.parent_x = px;
    g_x11.parent_y = py;

    uint32_t vals[4] = {
        static_cast<uint32_t>(px), static_cast<uint32_t>(py),
        static_cast<uint32_t>(pw), static_cast<uint32_t>(ph)
    };
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;

    for (auto* s : g_x11.live) {
        if (!s || s->window == XCB_NONE) continue;
        if (!s->visible) continue;
        xcb_configure_window(g_x11.conn, s->window, mask, vals);
    }
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
    if (conn) xcb_shm_detach(conn, buf.seg);
    shmdt(buf.data);
    buf.data = nullptr;
    buf.w = buf.h = 0;
    buf.size = 0;
}

// =====================================================================
// Window factory — top-level ARGB override-redirect child-of-mpv
// =====================================================================

// Caller must hold surface_mtx (for live list mutation if it's adding).
// Creates the window at the current parent position, depth=32 ARGB,
// override-redirect, input-passthrough, mapped immediately.
static xcb_window_t create_overlay_window_locked(int x, int y, int w, int h) {
    xcb_window_t win = xcb_generate_id(g_x11.conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                    XCB_CW_OVERRIDE_REDIRECT | XCB_CW_COLORMAP;
    uint32_t vals[4] = {0, 0, 1, g_x11.colormap};
    xcb_create_window(g_x11.conn, g_x11.argb_depth,
        win, g_x11.screen->root,
        x, y, w, h, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        g_x11.argb_visual, mask, vals);

    // Input-passthrough: empty input shape. All input goes to the mpv
    // parent window, where the X11 input thread picks it up.
    xcb_shape_rectangles(g_x11.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
        XCB_CLIP_ORDERING_UNSORTED, win, 0, 0, 0, nullptr);

    // Handle WM_DELETE_WINDOW if the WM ever targets this window
    xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, win,
        g_x11.wm_protocols, XCB_ATOM_ATOM, 32, 1,
        &g_x11.wm_delete_window);

    return win;
}

// =====================================================================
// Generic per-surface ops
// =====================================================================

static PlatformSurface* x11_alloc_surface() {
    auto* s = new PlatformSurface;
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
    if (!g_x11.conn || g_x11.parent == XCB_NONE) return s;

    int px = g_x11.parent_x, py = g_x11.parent_y;
    int pw = g_x11.pw > 0 ? g_x11.pw : 1;
    int ph = g_x11.ph > 0 ? g_x11.ph : 1;

    s->window = create_overlay_window_locked(px, py, pw, ph);
    s->gc = xcb_generate_id(g_x11.conn);
    xcb_create_gc(g_x11.conn, s->gc, s->window, 0, nullptr);
    s->pw = pw;
    s->ph = ph;
    s->visible = true;
    xcb_map_window(g_x11.conn, s->window);
    xcb_flush(g_x11.conn);

    g_x11.live.push_back(s);
    return s;
}

static void x11_free_surface(PlatformSurface* s) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);

    auto it = std::find(g_x11.live.begin(), g_x11.live.end(), s);
    if (it != g_x11.live.end()) g_x11.live.erase(it);

    for (auto& buf : s->bufs) shm_free(buf, g_x11.conn);

    if (s->window != XCB_NONE) xcb_unmap_window(g_x11.conn, s->window);
    if (s->gc != XCB_NONE) xcb_free_gc(g_x11.conn, s->gc);
    if (s->window != XCB_NONE) xcb_destroy_window(g_x11.conn, s->window);
    xcb_flush(g_x11.conn);
    delete s;
}

// X11 backend is software-only — no accelerated/shared-texture path.
static void x11_surface_present(PlatformSurface*, const CefAcceleratedPaintInfo&) {}

static void x11_surface_present_software(PlatformSurface* s,
                                         const CefRenderHandler::RectList& dirty,
                                         const void* buffer, int w, int h) {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
    if (s->window == XCB_NONE || !s->visible) return;

    auto& buf = s->bufs[s->buf_idx];
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

        xcb_shm_put_image(g_x11.conn, s->window, s->gc,
            w, h, rx, ry, rw, rh,
            rx, ry, g_x11.argb_depth,
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            0, buf.seg, 0);
    }

    s->buf_idx ^= 1;
    xcb_flush(g_x11.conn);
}

static void x11_surface_resize(PlatformSurface* s, int /*lw*/, int /*lh*/, int pw, int ph) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
    g_x11.pw = pw;
    g_x11.ph = ph;
    s->pw = pw;
    s->ph = ph;
    if (s->window == XCB_NONE) return;

    // Refresh parent position too — fullscreen and inter-monitor moves
    // both arrive through this path.
    int px = g_x11.parent_x, py = g_x11.parent_y, ppw = 0, pph = 0;
    if (query_parent_geometry(&px, &py, &ppw, &pph)) {
        g_x11.parent_x = px;
        g_x11.parent_y = py;
    }

    uint32_t vals[4] = {
        static_cast<uint32_t>(px), static_cast<uint32_t>(py),
        static_cast<uint32_t>(pw), static_cast<uint32_t>(ph)
    };
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    xcb_configure_window(g_x11.conn, s->window, mask, vals);
    xcb_flush(g_x11.conn);
}

static void x11_surface_set_visible(PlatformSurface* s, bool visible) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
    if (s->visible == visible) return;
    s->visible = visible;
    if (s->window == XCB_NONE) return;

    if (visible) {
        // Reposition to current parent geometry before mapping, so the
        // window appears in the right place if mpv moved while we were
        // hidden.
        int px = g_x11.parent_x, py = g_x11.parent_y;
        int pw = s->pw > 0 ? s->pw : (g_x11.pw > 0 ? g_x11.pw : 1);
        int ph = s->ph > 0 ? s->ph : (g_x11.ph > 0 ? g_x11.ph : 1);
        uint32_t vals[4] = {
            static_cast<uint32_t>(px), static_cast<uint32_t>(py),
            static_cast<uint32_t>(pw), static_cast<uint32_t>(ph)
        };
        uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        xcb_configure_window(g_x11.conn, s->window, mask, vals);
        xcb_map_window(g_x11.conn, s->window);
    } else {
        xcb_unmap_window(g_x11.conn, s->window);
    }
    xcb_flush(g_x11.conn);
}

// Stack the given surfaces above the mpv parent, in order bottom-to-top.
// X11 override-redirect windows are root-level, so we chain
// xcb_configure_window with SIBLING + STACK_MODE=Above over the parent.
static void x11_restack(PlatformSurface* const* ordered, size_t n) {
    if (!g_x11.conn || n == 0) return;
    std::lock_guard<std::mutex> lock(g_x11.surface_mtx);

    xcb_window_t prev = g_x11.parent;
    for (size_t i = 0; i < n; i++) {
        PlatformSurface* s = ordered[i];
        if (!s || s->window == XCB_NONE) continue;
        uint32_t vals[2] = { prev, XCB_STACK_MODE_ABOVE };
        uint32_t mask = XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;
        xcb_configure_window(g_x11.conn, s->window, mask, vals);
        prev = s->window;
    }
    xcb_flush(g_x11.conn);
}

// X11 backend has no per-surface alpha modulation through the compositor
// (no wp_alpha_modifier_v1 analogue; _NET_WM_WINDOW_OPACITY worked only
// on the legacy overlay window and is unreliable across compositors).
// Implement as a hard cut, matching the original overlay-fade fallback.
static void x11_fade_surface(PlatformSurface* s, float /*fade_sec*/,
                             std::function<void()> on_fade_start,
                             std::function<void()> on_complete) {
    if (on_fade_start) on_fade_start();
    x11_surface_set_visible(s, false);
    if (on_complete) on_complete();
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

static void hide_all_live_locked() {
    if (!g_x11.conn) return;
    for (auto* s : g_x11.live) {
        if (s && s->window != XCB_NONE)
            xcb_unmap_window(g_x11.conn, s->window);
    }
    xcb_flush(g_x11.conn);
}

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
    g_x11.pw = pw;
    g_x11.ph = ph;

    // Software rendering only.
    g_platform.shared_texture_supported = false;

    // Init input on mpv's parent window
    input::x11::init(g_x11.conn, g_x11.screen, g_x11.parent);
    input::x11::set_configure_callback([]() {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        sync_overlay_positions_locked();
    });
    input::x11::set_shutdown_callback([]() {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        hide_all_live_locked();
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
    // Hide any straggler surface windows immediately so they don't linger
    // during shutdown. Browsers normally frees its surfaces before this
    // runs; this is defensive.
    if (g_x11.conn) {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        hide_all_live_locked();
    }

    idle_inhibit::cleanup();
    input::x11::cleanup();

    // Free any surface that outlived Browsers (defensive — Browsers dtor
    // should already have freed them).
    {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        for (auto* s : g_x11.live) {
            if (!s) continue;
            for (auto& buf : s->bufs) shm_free(buf, g_x11.conn);
            if (s->gc != XCB_NONE) xcb_free_gc(g_x11.conn, s->gc);
            if (s->window != XCB_NONE) xcb_destroy_window(g_x11.conn, s->window);
            delete s;
        }
        g_x11.live.clear();
    }

    if (g_x11.colormap != XCB_NONE && g_x11.conn)
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
        .post_window_cleanup = nullptr,
        .alloc_surface = x11_alloc_surface,
        .free_surface = x11_free_surface,
        .surface_present = x11_surface_present,
        .surface_present_software = x11_surface_present_software,
        .surface_resize = x11_surface_resize,
        .surface_set_visible = x11_surface_set_visible,
        .restack = x11_restack,
        .fade_surface = x11_fade_surface,
        // X11 popup not implemented (pre-existing gap).
        .popup_show = [](PlatformSurface*, const Platform::PopupRequest&) {},
        .popup_hide = [](PlatformSurface*) {},
        .popup_present = [](PlatformSurface*, const CefAcceleratedPaintInfo&, int, int) {},
        .popup_present_software = [](PlatformSurface*, const void*, int, int, int, int) {},
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
        .set_theme_color = [](const Color&) {},
        .shared_texture_supported = false,
        .clipboard_read_text_async = nullptr,
        .open_external_url = open_url_linux::open,
    };
}
