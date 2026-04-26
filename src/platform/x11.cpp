#include "common.h"
#include "cef/cef_client.h"
#include "browser/browsers.h"
#include "browser/web_browser.h"
#include "browser/overlay_browser.h"
#include "browser/about_browser.h"
#include "idle_inhibit_linux.h"
#include "open_url_linux.h"
#include "input/input_x11.h"
#include "mpv/event.h"

#include <mpv/render.h>
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/shape.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
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

struct X11State {
    std::mutex surface_mtx;
    std::mutex render_mtx;
    std::condition_variable render_cv;
    xcb_connection_t* conn = nullptr;
    int screen_num = 0;
    xcb_screen_t* screen = nullptr;

    // parent is the RGB WM-managed app window. In self-composite mode, the
    // app composites mpv + CEF into it via SW. In overlay mode, mpv renders
    // natively into it (via wid) and CEF content lands on cef_overlay above.
    xcb_window_t parent = XCB_NONE;
    xcb_window_t mpv_window = XCB_NONE;
    bool owns_parent = false;

    // Overlay-mode only: ARGB override-redirect sibling stacked above parent.
    // Holds the CEF composite (main + overlay + about blended over alpha=0).
    // Input-shape empty → clicks pass through to parent, so input wiring is
    // unchanged.
    xcb_window_t cef_overlay = XCB_NONE;

    // True ⇒ SW self-composite mode (vo=libmpv), false ⇒ ARGB overlay over
    // native mpv (default). Set via x11_set_self_composite() before init.
    bool self_composite = true;

    // Final composed X11 frame.
    ShmBuffer final_buf;
    xcb_gcontext_t final_gc = XCB_NONE;

    // Main browser layer.
    xcb_window_t cef_window = XCB_NONE;
    ShmBuffer cef_bufs[2];
    int cef_buf_idx = 0;
    std::vector<uint8_t> main_layer;

    // Overlay browser layer.
    xcb_window_t overlay_window = XCB_NONE;
    ShmBuffer overlay_bufs[2];
    int overlay_buf_idx = 0;
    bool overlay_visible = false;
    std::vector<uint8_t> overlay_layer;
    uint8_t overlay_alpha = 255;

    // About browser layer.
    xcb_window_t about_window = XCB_NONE;
    ShmBuffer about_bufs[2];
    int about_buf_idx = 0;
    bool about_visible = false;
    std::vector<uint8_t> about_layer;

    // Graphics contexts (one per overlay window, reused across frames)
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
    std::vector<uint8_t> mpv_layer;
    std::vector<uint8_t> composed_layer;
    bool main_ready = true;
    bool overlay_ready = true;
    bool about_ready = true;
    bool main_needs_full_upload = false;
    bool overlay_needs_full_upload = false;
    bool about_needs_full_upload = false;

    // Fade
    bool transitioning = false;
    bool minimized = false;
    int transition_pw = 0, transition_ph = 0;
    int pending_lw = 0, pending_lh = 0;
    int expected_w = 0, expected_h = 0;
    bool was_fullscreen = false;
    std::mutex fade_mtx;
    std::atomic<uint64_t> fade_generation{0};
    std::thread fade_thread;

    // Atoms
    xcb_atom_t net_wm_opacity = XCB_NONE;
    xcb_atom_t net_wm_window_type = XCB_NONE;
    xcb_atom_t net_wm_window_type_notification = XCB_NONE;
    xcb_atom_t net_wm_state = XCB_NONE;
    xcb_atom_t net_wm_state_above = XCB_NONE;
    xcb_atom_t net_wm_state_skip_taskbar = XCB_NONE;
    xcb_atom_t net_wm_state_skip_pager = XCB_NONE;
    xcb_atom_t net_wm_state_fullscreen = XCB_NONE;
    xcb_atom_t net_wm_state_maximized_vert = XCB_NONE;
    xcb_atom_t net_wm_state_maximized_horz = XCB_NONE;
    xcb_atom_t net_wm_name = XCB_NONE;
    xcb_atom_t net_wm_icon = XCB_NONE;
    xcb_atom_t net_wm_pid = XCB_NONE;
    xcb_atom_t utf8_string = XCB_NONE;
    xcb_atom_t wm_protocols = XCB_NONE;
    xcb_atom_t wm_delete_window = XCB_NONE;
    xcb_atom_t net_wm_window_type_utility = XCB_NONE;
    xcb_atom_t motif_wm_hints = XCB_NONE;

    // Parent position (for overlay positioning)
    int parent_x = 0, parent_y = 0;

    mpv_render_context* render_ctx = nullptr;
    bool render_requested = false;
    bool render_stop = false;
    std::thread render_thread;
};

static X11State g_x11;

static constexpr const char* kAppId = "org.jellyfin.JellyfinDesktop";
static constexpr const char* kAppTitle = "Jellyfin Desktop";

static float x11_get_scale();
static void x11_overlay_resize(int, int, int, int);
static void x11_about_resize(int, int, int, int);

static void x11_cancel_fade() {
    std::thread old_thread;
    {
        std::lock_guard<std::mutex> lock(g_x11.fade_mtx);
        g_x11.fade_generation.fetch_add(1, std::memory_order_relaxed);
        if (g_x11.fade_thread.joinable())
            old_thread = std::move(g_x11.fade_thread);
    }
    if (old_thread.joinable())
        old_thread.join();
}

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

static bool ensure_x11_connection() {
    if (g_x11.conn) return true;

    g_x11.conn = xcb_connect(nullptr, &g_x11.screen_num);
    if (xcb_connection_has_error(g_x11.conn)) {
        fprintf(stderr, "Failed to connect to X11\n");
        xcb_disconnect(g_x11.conn);
        g_x11.conn = nullptr;
        return false;
    }

    auto setup = xcb_get_setup(g_x11.conn);
    auto screen_iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < g_x11.screen_num; i++)
        xcb_screen_next(&screen_iter);
    g_x11.screen = screen_iter.data;

    g_x11.net_wm_opacity = intern_atom(g_x11.conn, "_NET_WM_WINDOW_OPACITY");
    g_x11.net_wm_window_type = intern_atom(g_x11.conn, "_NET_WM_WINDOW_TYPE");
    g_x11.net_wm_window_type_notification = intern_atom(g_x11.conn, "_NET_WM_WINDOW_TYPE_NOTIFICATION");
    g_x11.net_wm_state = intern_atom(g_x11.conn, "_NET_WM_STATE");
    g_x11.net_wm_state_above = intern_atom(g_x11.conn, "_NET_WM_STATE_ABOVE");
    g_x11.net_wm_state_skip_taskbar = intern_atom(g_x11.conn, "_NET_WM_STATE_SKIP_TASKBAR");
    g_x11.net_wm_state_skip_pager = intern_atom(g_x11.conn, "_NET_WM_STATE_SKIP_PAGER");
    g_x11.net_wm_state_fullscreen = intern_atom(g_x11.conn, "_NET_WM_STATE_FULLSCREEN");
    g_x11.net_wm_state_maximized_vert = intern_atom(g_x11.conn, "_NET_WM_STATE_MAXIMIZED_VERT");
    g_x11.net_wm_state_maximized_horz = intern_atom(g_x11.conn, "_NET_WM_STATE_MAXIMIZED_HORZ");
    g_x11.net_wm_name = intern_atom(g_x11.conn, "_NET_WM_NAME");
    g_x11.net_wm_icon = intern_atom(g_x11.conn, "_NET_WM_ICON");
    g_x11.net_wm_pid = intern_atom(g_x11.conn, "_NET_WM_PID");
    g_x11.utf8_string = intern_atom(g_x11.conn, "UTF8_STRING");
    g_x11.wm_protocols = intern_atom(g_x11.conn, "WM_PROTOCOLS");
    g_x11.net_wm_window_type_utility = intern_atom(g_x11.conn, "_NET_WM_WINDOW_TYPE_UTILITY");
    g_x11.motif_wm_hints = intern_atom(g_x11.conn, "_MOTIF_WM_HINTS");
    g_x11.wm_delete_window = intern_atom(g_x11.conn, "WM_DELETE_WINDOW");
    return true;
}

static bool in_triangle(float px, float py,
                        float ax, float ay, float bx, float by, float cx, float cy) {
    float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    bool has_neg = d1 < 0 || d2 < 0 || d3 < 0;
    bool has_pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(has_neg && has_pos);
}

static uint32_t jellyfin_icon_pixel(float x, float y, float coverage) {
    float t = std::fmin(1.0f, std::fmax(0.0f, (x + y + 1.7f) / 3.4f));
    auto lerp = [t](uint8_t a, uint8_t b) -> uint8_t {
        return static_cast<uint8_t>(std::lround(a + (b - a) * t));
    };
    uint8_t r = lerp(0xaa, 0x00);
    uint8_t g = lerp(0x5c, 0xa4);
    uint8_t b = lerp(0xc3, 0xdc);
    uint8_t a = static_cast<uint8_t>(std::lround(255.0f * coverage));
    return r | (g << 8) | (b << 16) | (static_cast<uint32_t>(a) << 24);
}

static void append_jellyfin_icon(std::vector<uint32_t>& out, int size) {
    out.push_back(static_cast<uint32_t>(size));
    out.push_back(static_cast<uint32_t>(size));

    constexpr float samples[3] = {0.2f, 0.5f, 0.8f};
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            int covered = 0;
            float cx = 0.0f;
            float cy = 0.0f;
            for (float sy : samples) {
                for (float sx : samples) {
                    float x = ((px + sx) / size) * 2.0f - 1.0f;
                    float y = ((py + sy) / size) * 2.0f - 1.0f;
                    bool outer = in_triangle(x, y, 0.0f, -0.92f, -0.86f, 0.76f, 0.86f, 0.76f);
                    bool hole = in_triangle(x, y, 0.0f, -0.47f, -0.54f, 0.56f, 0.54f, 0.56f);
                    bool inner = in_triangle(x, y, 0.0f, -0.18f, -0.34f, 0.47f, 0.34f, 0.47f);
                    if ((outer && !hole) || inner) {
                        covered++;
                        cx += x;
                        cy += y;
                    }
                }
            }
            if (!covered) {
                out.push_back(0);
            } else {
                out.push_back(jellyfin_icon_pixel(cx / covered, cy / covered, covered / 9.0f));
            }
        }
    }
}

static void set_jellyfin_window_metadata(xcb_window_t win) {
    const std::string wm_class = std::string(kAppId) + '\0' + kAppId;
    xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, win,
        XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, wm_class.size() + 1, wm_class.c_str());
    xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, win,
        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(kAppTitle), kAppTitle);
    xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, win,
        g_x11.net_wm_name, g_x11.utf8_string, 8, strlen(kAppTitle), kAppTitle);

    uint32_t pid = static_cast<uint32_t>(getpid());
    xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, win,
        g_x11.net_wm_pid, XCB_ATOM_CARDINAL, 32, 1, &pid);
    xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, win,
        g_x11.wm_protocols, XCB_ATOM_ATOM, 32, 1, &g_x11.wm_delete_window);

    std::vector<uint32_t> icon;
    icon.reserve(2 + 64 * 64 + 2 + 128 * 128);
    append_jellyfin_icon(icon, 64);
    append_jellyfin_icon(icon, 128);
    xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, win,
        g_x11.net_wm_icon, XCB_ATOM_CARDINAL, 32, icon.size(), icon.data());
}

static void set_parent_state_property(std::initializer_list<xcb_atom_t> states) {
    std::vector<xcb_atom_t> atoms;
    for (xcb_atom_t atom : states) {
        if (atom != XCB_NONE) atoms.push_back(atom);
    }
    xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, g_x11.parent,
        g_x11.net_wm_state, XCB_ATOM_ATOM, 32, atoms.size(), atoms.data());
}

static void send_parent_state(xcb_atom_t atom, bool enabled) {
    if (!g_x11.conn || g_x11.parent == XCB_NONE || atom == XCB_NONE) return;
    xcb_client_message_event_t ev{};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = g_x11.parent;
    ev.type = g_x11.net_wm_state;
    ev.format = 32;
    ev.data.data32[0] = enabled ? 1 : 0;
    ev.data.data32[1] = atom;
    ev.data.data32[2] = 0;
    ev.data.data32[3] = 1;
    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_send_event(g_x11.conn, false, g_x11.screen->root, mask,
        reinterpret_cast<const char*>(&ev));
    xcb_flush(g_x11.conn);
}

static bool x11_prepare_window(int w, int h, int x, int y, bool maximized) {
    if (!ensure_x11_connection()) return false;
    if (g_x11.parent != XCB_NONE) return true;

    uint16_t ww = static_cast<uint16_t>(w > 0 ? w : 1280);
    uint16_t hh = static_cast<uint16_t>(h > 0 ? h : 720);
    int px = x >= 0 ? x : 0;
    int py = y >= 0 ? y : 0;

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t events = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    uint32_t vals[2] = {0x101010, events};

    g_x11.parent = xcb_generate_id(g_x11.conn);
    g_x11.owns_parent = true;
    xcb_create_window(g_x11.conn, XCB_COPY_FROM_PARENT,
        g_x11.parent, g_x11.screen->root,
        px, py, ww, hh,
        0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        g_x11.screen->root_visual, mask, vals);

    set_jellyfin_window_metadata(g_x11.parent);
    if (maximized) {
        set_parent_state_property({
            g_x11.net_wm_state_maximized_vert,
            g_x11.net_wm_state_maximized_horz,
        });
    }
    xcb_map_window(g_x11.conn, g_x11.parent);

    // Overlay mode: WM-managed ARGB window tied to parent via
    // WM_TRANSIENT_FOR. The WM natively raises/lowers the transient with
    // its parent, so when another top-level covers parent it covers the
    // overlay too — no restack-on-every-event bookkeeping needed.
    if (!g_x11.self_composite) {
        g_x11.argb_visual = find_argb_visual(g_x11.screen, &g_x11.argb_depth);
        if (!g_x11.argb_visual) {
            fprintf(stderr, "overlay mode requires a 32-bit ARGB visual\n");
            return false;
        }
        g_x11.colormap = xcb_generate_id(g_x11.conn);
        xcb_create_colormap(g_x11.conn, XCB_COLORMAP_ALLOC_NONE,
            g_x11.colormap, g_x11.screen->root, g_x11.argb_visual);

        uint32_t ov_mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                           XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
        uint32_t ov_vals[4] = {0, 0, XCB_EVENT_MASK_EXPOSURE, g_x11.colormap};
        g_x11.cef_overlay = xcb_generate_id(g_x11.conn);
        xcb_create_window(g_x11.conn, g_x11.argb_depth,
            g_x11.cef_overlay, g_x11.screen->root,
            px, py, ww, hh, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT,
            g_x11.argb_visual, ov_mask, ov_vals);

        // Tell the WM: this window rides parent's z-order.
        xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, g_x11.cef_overlay,
            XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &g_x11.parent);

        // Utility window hint; keeps WM from giving it decorations or a
        // taskbar/pager entry on most WMs.
        xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, g_x11.cef_overlay,
            g_x11.net_wm_window_type, XCB_ATOM_ATOM, 32, 1,
            &g_x11.net_wm_window_type_utility);
        xcb_atom_t states[2] = {
            g_x11.net_wm_state_skip_taskbar,
            g_x11.net_wm_state_skip_pager,
        };
        xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, g_x11.cef_overlay,
            g_x11.net_wm_state, XCB_ATOM_ATOM, 32, 2, states);

        // Kill every decoration (title bar, border, resize handles). Motif
        // hints struct: flags, funcs, decorations, input_mode, status. Flag
        // bit 1 = decorations; decorations=0 = none. Honored by kwin/mutter/
        // xfwm/fluxbox/openbox etc.
        uint32_t motif_hints[5] = {2, 0, 0, 0, 0};
        xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, g_x11.cef_overlay,
            g_x11.motif_wm_hints, g_x11.motif_wm_hints, 32, 5, motif_hints);

        // WM_HINTS: InputHint=set, input=False. Tells the WM to never
        // transfer keyboard focus to the overlay, even when raised with
        // parent — focus stays on the main (mpv-carrying) window.
        uint32_t wm_hints[9] = {1, 0, 0, 0, 0, 0, 0, 0, 0};
        xcb_change_property(g_x11.conn, XCB_PROP_MODE_REPLACE, g_x11.cef_overlay,
            XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, 9, wm_hints);

        // Empty input shape → clicks fall through to parent.
        xcb_shape_rectangles(g_x11.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
            XCB_CLIP_ORDERING_UNSORTED, g_x11.cef_overlay, 0, 0, 0, nullptr);

        xcb_map_window(g_x11.conn, g_x11.cef_overlay);
    }

    xcb_flush(g_x11.conn);
    return true;
}

void x11_set_self_composite(bool enabled) {
    g_x11.self_composite = enabled;
}

static int64_t x11_native_window_id() {
    return static_cast<int64_t>(g_x11.parent);
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

static void configure_layer_window(xcb_window_t win, int x, int y, int w, int h) {
    if (win == XCB_NONE) return;
    uint32_t vals[5] = {
        static_cast<uint32_t>(x), static_cast<uint32_t>(y),
        static_cast<uint32_t>(w), static_cast<uint32_t>(h),
        XCB_STACK_MODE_ABOVE,
    };
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                    XCB_CONFIG_WINDOW_STACK_MODE;
    xcb_configure_window(g_x11.conn, win, mask, vals);
}

// Keep root-level ARGB CEF layers aligned with the app window.
static void sync_overlay_positions() {
    int px, py, pw, ph;
    if (!query_parent_geometry(&px, &py, &pw, &ph)) return;

    if (g_x11.cef_window != XCB_NONE)
        configure_layer_window(g_x11.cef_window, px, py, pw, ph);
    if (g_x11.overlay_window != XCB_NONE && g_x11.overlay_visible)
        configure_layer_window(g_x11.overlay_window, px, py, pw, ph);
    if (g_x11.about_window != XCB_NONE && g_x11.about_visible)
        configure_layer_window(g_x11.about_window, px, py, pw, ph);
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

struct DirtyBounds {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

static bool compute_dirty_bounds(const CefRenderHandler::RectList& dirty,
                                 int w, int h, DirtyBounds& out) {
    if (w <= 0 || h <= 0) return false;
    if (dirty.empty()) {
        out = {0, 0, w, h};
        return true;
    }

    int min_x = w;
    int min_y = h;
    int max_x = 0;
    int max_y = 0;
    for (const auto& rect : dirty) {
        int x1 = rect.x;
        int y1 = rect.y;
        int x2 = rect.x + rect.width;
        int y2 = rect.y + rect.height;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > w) x2 = w;
        if (y2 > h) y2 = h;
        if (x2 <= x1 || y2 <= y1) continue;
        if (x1 < min_x) min_x = x1;
        if (y1 < min_y) min_y = y1;
        if (x2 > max_x) max_x = x2;
        if (y2 > max_y) max_y = y2;
    }
    if (max_x <= min_x || max_y <= min_y) return false;
    out = {min_x, min_y, max_x - min_x, max_y - min_y};
    return true;
}

static bool ensure_layer(std::vector<uint8_t>& layer, int w, int h, bool clear = false) {
    if (w <= 0 || h <= 0) return false;
    size_t size = static_cast<size_t>(w) * h * 4;
    if (layer.size() != size) {
        layer.assign(size, 0);
    } else if (clear) {
        std::fill(layer.begin(), layer.end(), 0);
    }
    return true;
}

static void copy_cef_dirty_to_layer(std::vector<uint8_t>& layer,
                                    const CefRenderHandler::RectList& dirty,
                                    const void* buffer, int w, int h) {
    if (!ensure_layer(layer, w, h)) return;

    DirtyBounds bounds;
    if (!compute_dirty_bounds(dirty, w, h, bounds)) return;

    int stride = w * 4;
    const auto* src = static_cast<const uint8_t*>(buffer);
    for (int row = bounds.y; row < bounds.y + bounds.h; row++) {
        memcpy(layer.data() + row * stride + bounds.x * 4,
               src + row * stride + bounds.x * 4,
               bounds.w * 4);
    }
}

static void copy_cef_full_to_layer(std::vector<uint8_t>& layer,
                                   const void* buffer, int w, int h) {
    if (!ensure_layer(layer, w, h)) return;
    memcpy(layer.data(), buffer, static_cast<size_t>(w) * h * 4);
}

// Blend `src` (CEF-delivered BGRA, straight alpha) over `dst`.
// premul_output=false: dst is RGB (self-composite target ignores alpha) —
//   straight-alpha OVER into an opaque background, forces dst.a=255.
// premul_output=true: dst is ARGB (overlay target requires premultiplied) —
//   accumulates premultiplied OVER, letting the compositor show mpv through
//   where dst.a < 255.
static void blend_layer(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src,
                        uint8_t layer_alpha = 255, bool premul_output = false) {
    if (dst.size() != src.size() || layer_alpha == 0) return;
    size_t pixels = dst.size() / 4;
    for (size_t i = 0; i < pixels; i++) {
        uint8_t* d = dst.data() + i * 4;
        const uint8_t* s = src.data() + i * 4;
        uint32_t a = (static_cast<uint32_t>(s[3]) * layer_alpha + 127) / 255;
        if (a == 0) continue;
        uint32_t inv = 255 - a;
        if (!premul_output) {
            if (a == 255) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                continue;
            }
            d[0] = static_cast<uint8_t>((static_cast<uint32_t>(s[0]) * a + d[0] * inv + 127) / 255);
            d[1] = static_cast<uint8_t>((static_cast<uint32_t>(s[1]) * a + d[1] * inv + 127) / 255);
            d[2] = static_cast<uint8_t>((static_cast<uint32_t>(s[2]) * a + d[2] * inv + 127) / 255);
            d[3] = 255;
        } else {
            // Premultiply straight-alpha src, then OVER onto premul dst.
            uint32_t pr = (static_cast<uint32_t>(s[0]) * a + 127) / 255;
            uint32_t pg = (static_cast<uint32_t>(s[1]) * a + 127) / 255;
            uint32_t pb = (static_cast<uint32_t>(s[2]) * a + 127) / 255;
            d[0] = static_cast<uint8_t>(pr + (static_cast<uint32_t>(d[0]) * inv + 127) / 255);
            d[1] = static_cast<uint8_t>(pg + (static_cast<uint32_t>(d[1]) * inv + 127) / 255);
            d[2] = static_cast<uint8_t>(pb + (static_cast<uint32_t>(d[2]) * inv + 127) / 255);
            d[3] = static_cast<uint8_t>(a + (static_cast<uint32_t>(d[3]) * inv + 127) / 255);
        }
    }
}

// Render mpv into g_x11.mpv_layer at pw/ph. Render-thread only.
// mpv_layer is exclusively owned by the render thread — no lock required.
static bool render_mpv_unlocked(int pw, int ph) {
    if (!g_x11.render_ctx || pw <= 0 || ph <= 0) return false;
    if (!ensure_layer(g_x11.mpv_layer, pw, ph, false)) return false;

    int size[2] = {pw, ph};
    size_t stride = static_cast<size_t>(pw) * 4;
    const char* format = "bgr0";
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>(format)},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, g_x11.mpv_layer.data()},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    int err = mpv_render_context_render(g_x11.render_ctx, params);
    return err >= 0;
}

static void request_render_frame() {
    {
        std::lock_guard<std::mutex> lock(g_x11.render_mtx);
        g_x11.render_requested = true;
    }
    g_x11.render_cv.notify_one();
}

static void render_update_callback(void*) {
    request_render_frame();
}

static void handle_parent_configure() {
    int x = 0, y = 0, w = 0, h = 0;
    if (!query_parent_geometry(&x, &y, &w, &h) || w <= 0 || h <= 0) return;

    bool size_changed = false;
    bool pos_changed = false;
    {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        pos_changed = g_x11.parent_x != x || g_x11.parent_y != y;
        g_x11.parent_x = x;
        g_x11.parent_y = y;
        size_changed = g_x11.pw != w || g_x11.ph != h;
        if (size_changed) {
            g_x11.pw = w;
            g_x11.ph = h;
            // mpv_layer / composed_layer are render-thread-owned; don't touch
            // them here. CEF-owned layers get resized when the next CEF paint
            // runs with full_upload set.
            ensure_layer(g_x11.main_layer, w, h, true);
            ensure_layer(g_x11.overlay_layer, w, h, true);
            ensure_layer(g_x11.about_layer, w, h, true);
            g_x11.main_needs_full_upload = true;
            g_x11.overlay_needs_full_upload = true;
            g_x11.about_needs_full_upload = true;
            mpv::set_window_pixels(w, h);
        }
    }

    // Overlay mode: keep the ARGB overlay glued to the app window on every
    // move or resize — not just size changes, since drag-only moves keep pw/ph
    // constant but shift the absolute position. Stacking is the WM's job
    // via WM_TRANSIENT_FOR, so don't force STACK_MODE here.
    if (!g_x11.self_composite && g_x11.cef_overlay != XCB_NONE &&
        (size_changed || pos_changed)) {
        uint32_t vals[4] = {
            static_cast<uint32_t>(x), static_cast<uint32_t>(y),
            static_cast<uint32_t>(w), static_cast<uint32_t>(h),
        };
        xcb_configure_window(g_x11.conn, g_x11.cef_overlay,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            vals);
        xcb_flush(g_x11.conn);
    }

    if (size_changed) {
        float scale = x11_get_scale();
        if (scale <= 0.f) scale = 1.0f;
        int lw = static_cast<int>(w / scale);
        int lh = static_cast<int>(h / scale);
        if (g_web_browser && g_web_browser->browser())
            g_web_browser->resize(lw, lh, w, h);
        if (g_overlay_browser && g_overlay_browser->browser()) {
            g_overlay_browser->resize(lw, lh, w, h);
            x11_overlay_resize(lw, lh, w, h);
        }
        if (g_about_browser && g_about_browser->browser()) {
            g_about_browser->resize(lw, lh, w, h);
            x11_about_resize(lw, lh, w, h);
        }
    }
    request_render_frame();
}

static void upload_dirty_region(xcb_window_t window, xcb_gcontext_t gc,
                                ShmBuffer& buf,
                                const CefRenderHandler::RectList& dirty,
                                const void* buffer, int w, int h,
                                bool force_full_upload) {
    if (!shm_alloc(buf, g_x11.conn, w, h)) return;

    DirtyBounds bounds;
    if (force_full_upload) {
        bounds = {0, 0, w, h};
    } else if (!compute_dirty_bounds(dirty, w, h, bounds)) {
        return;
    }

    int stride = w * 4;
    const auto* src = static_cast<const uint8_t*>(buffer);
    for (int row = bounds.y; row < bounds.y + bounds.h; row++) {
        memcpy(buf.data + row * stride + bounds.x * 4,
               src + row * stride + bounds.x * 4,
               bounds.w * 4);
    }

    xcb_shm_put_image(g_x11.conn, window, gc,
        w, h, bounds.x, bounds.y, bounds.w, bounds.h,
        bounds.x, bounds.y, g_x11.argb_depth,
        XCB_IMAGE_FORMAT_Z_PIXMAP,
        0, buf.seg, 0);
}

// =====================================================================
// Present CEF software -- main browser
// =====================================================================

static void set_minimized_locked(bool minimized) {
    if (g_x11.minimized == minimized) return;
    g_x11.minimized = minimized;
    request_render_frame();
}

static void x11_present_software(const CefRenderHandler::RectList& dirty,
                                 const void* buffer, int w, int h) {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        if (g_x11.parent == XCB_NONE || g_x11.minimized) return;

        if (g_x11.main_needs_full_upload || w != g_x11.pw || h != g_x11.ph)
            copy_cef_full_to_layer(g_x11.main_layer, buffer, w, h);
        else
            copy_cef_dirty_to_layer(g_x11.main_layer, dirty, buffer, w, h);
        g_x11.main_ready = true;
        g_x11.main_needs_full_upload = false;
    }
    request_render_frame();
}

// =====================================================================
// Present CEF software -- overlay browser
// =====================================================================

static void x11_overlay_present_software(const CefRenderHandler::RectList& dirty,
                                         const void* buffer, int w, int h) {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        if (g_x11.parent == XCB_NONE || !g_x11.overlay_visible || g_x11.minimized) return;

        if (g_x11.overlay_needs_full_upload || w != g_x11.pw || h != g_x11.ph)
            copy_cef_full_to_layer(g_x11.overlay_layer, buffer, w, h);
        else
            copy_cef_dirty_to_layer(g_x11.overlay_layer, dirty, buffer, w, h);
        g_x11.overlay_ready = true;
        g_x11.overlay_needs_full_upload = false;
    }
    request_render_frame();
}

// =====================================================================
// Resize
// =====================================================================

static void x11_resize(int lw, int lh, int pw, int ph) {
    {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        bool size_changed = g_x11.pw != pw || g_x11.ph != ph;
        g_x11.pw = pw;
        g_x11.ph = ph;
        if (size_changed) {
            g_x11.main_ready = false;
            g_x11.main_needs_full_upload = true;
            if (g_x11.overlay_visible) g_x11.overlay_ready = false;
            if (g_x11.overlay_visible) g_x11.overlay_needs_full_upload = true;
            if (g_x11.about_visible) g_x11.about_ready = false;
            if (g_x11.about_visible) g_x11.about_needs_full_upload = true;
            // mpv_layer / composed_layer are render-thread-owned.
            ensure_layer(g_x11.main_layer, pw, ph, true);
            ensure_layer(g_x11.overlay_layer, pw, ph, true);
            ensure_layer(g_x11.about_layer, pw, ph, true);
            if (g_x11.parent != XCB_NONE) {
                uint32_t vals[2] = {static_cast<uint32_t>(pw), static_cast<uint32_t>(ph)};
                xcb_configure_window(g_x11.conn, g_x11.parent,
                                     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                                     vals);
            }
        }
    }
    request_render_frame();
}

static void x11_overlay_resize(int, int, int, int) {
    request_render_frame();
}

// =====================================================================
// Present CEF software -- about browser
// =====================================================================

static void x11_about_present_software(const CefRenderHandler::RectList& dirty,
                                       const void* buffer, int w, int h) {
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        if (g_x11.parent == XCB_NONE || !g_x11.about_visible || g_x11.minimized) return;

        if (g_x11.about_needs_full_upload || w != g_x11.pw || h != g_x11.ph)
            copy_cef_full_to_layer(g_x11.about_layer, buffer, w, h);
        else
            copy_cef_dirty_to_layer(g_x11.about_layer, dirty, buffer, w, h);
        g_x11.about_ready = true;
        g_x11.about_needs_full_upload = false;
    }
    request_render_frame();
}

static void x11_about_resize(int, int, int, int) {
    request_render_frame();
}

static void x11_set_about_visible(bool visible) {
    int pw = 0, ph = 0;
    float scale = 1.0f;
    {
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        if (g_x11.about_visible == visible) return;
        g_x11.about_visible = visible;
        // Clear the stale layer so the previous open's bitmap doesn't show
        // through before the new browser paints its first frame.
        if (visible) {
            std::fill(g_x11.about_layer.begin(), g_x11.about_layer.end(), 0);
            g_x11.about_needs_full_upload = true;
        }
        pw = g_x11.pw;
        ph = g_x11.ph;
        scale = g_x11.cached_scale > 0 ? g_x11.cached_scale : 1.0f;
    }
    request_render_frame();

    if (visible && !g_x11.minimized) {
        // Force the about browser to match the compositor window size now,
        // so its first paint matches g_x11.pw/ph exactly and composites.
        if (g_about_browser && g_about_browser->browser() && pw > 0 && ph > 0) {
            int lw = static_cast<int>(pw / scale);
            int lh = static_cast<int>(ph / scale);
            g_about_browser->resize(lw, lh, pw, ph);
        }
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
        g_x11.overlay_alpha = 255;
    }
    request_render_frame();

    // Route keyboard focus to the active browser
    auto main = g_web_browser ? g_web_browser->browser() : nullptr;
    auto ovl  = g_overlay_browser ? g_overlay_browser->browser() : nullptr;
    if (visible && !g_x11.minimized) {
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
    x11_cancel_fade();

    uint64_t generation = g_x11.fade_generation.load(std::memory_order_relaxed);
    std::thread fade_thread([fade_sec, generation,
                             on_fade_start = std::move(on_fade_start),
                             on_complete = std::move(on_complete)]() {
        if (on_fade_start) on_fade_start();

        int fps = g_display_hz.load(std::memory_order_relaxed);
        int total_frames = static_cast<int>(fade_sec * fps);
        if (total_frames < 1) total_frames = 1;
        auto frame_duration = std::chrono::microseconds(1000000 / fps);

        for (int i = 1; i <= total_frames; i++) {
            if (g_x11.fade_generation.load(std::memory_order_relaxed) != generation)
                return;

            float t = static_cast<float>(i) / total_frames;
            uint8_t alpha = static_cast<uint8_t>(std::lround((1.0f - t) * 255.0f));

            {
                std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
                if (!g_x11.overlay_visible) break;
                g_x11.overlay_alpha = alpha;
            }
            request_render_frame();
            std::this_thread::sleep_for(frame_duration);
        }

        if (g_x11.fade_generation.load(std::memory_order_relaxed) != generation)
            return;
        x11_set_overlay_visible(false);
        if (on_complete) on_complete();
    });

    std::lock_guard<std::mutex> lock(g_x11.fade_mtx);
    g_x11.fade_thread = std::move(fade_thread);
}

// =====================================================================
// Fullscreen
// =====================================================================

static void x11_set_fullscreen(bool fullscreen) {
    if (g_x11.owns_parent)
        send_parent_state(g_x11.net_wm_state_fullscreen, fullscreen);
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

static bool x11_init(mpv_handle* mpv) {
    if (!ensure_x11_connection()) return false;
    if (g_x11.parent == XCB_NONE && !x11_prepare_window(1280, 720, -1, -1, false))
        return false;

    // Check for SHM extension
    auto shm_cookie = xcb_shm_query_version(g_x11.conn);
    auto* shm_reply = xcb_shm_query_version_reply(g_x11.conn, shm_cookie, nullptr);
    if (!shm_reply) {
        fprintf(stderr, "X11 MIT-SHM extension not available\n");
        return false;
    }
    free(shm_reply);

    int px = 0, py = 0, pw = 1, ph = 1;
    query_parent_geometry(&px, &py, &pw, &ph);
    g_x11.parent_x = px;
    g_x11.parent_y = py;
    g_x11.pw = pw;
    g_x11.ph = ph;
    ensure_layer(g_x11.main_layer, pw, ph, true);
    ensure_layer(g_x11.overlay_layer, pw, ph, true);
    ensure_layer(g_x11.about_layer, pw, ph, true);

    // Align overlay to parent's on-screen geometry now. The queued
    // ConfigureNotify would otherwise be a no-op because we just seeded
    // parent_x/y to the WM-placed position. WM handles stacking for
    // transient windows; we only configure position+size.
    if (!g_x11.self_composite && g_x11.cef_overlay != XCB_NONE) {
        uint32_t vals[4] = {
            static_cast<uint32_t>(px), static_cast<uint32_t>(py),
            static_cast<uint32_t>(pw), static_cast<uint32_t>(ph),
        };
        xcb_configure_window(g_x11.conn, g_x11.cef_overlay,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            vals);
    }

    // Compose target window (parent in self-composite, cef_overlay in overlay mode).
    xcb_window_t target = g_x11.self_composite ? g_x11.parent : g_x11.cef_overlay;
    g_x11.final_gc = xcb_generate_id(g_x11.conn);
    xcb_create_gc(g_x11.conn, g_x11.final_gc, target, 0, nullptr);

    xcb_flush(g_x11.conn);

    if (g_x11.self_composite) {
        const char* api = MPV_RENDER_API_TYPE_SW;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(api)},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        int err = mpv_render_context_create(&g_x11.render_ctx, mpv, params);
        if (err < 0) {
            fprintf(stderr, "mpv_render_context_create(sw) failed: %s\n", mpv_error_string(err));
            return false;
        }
        mpv_render_context_set_update_callback(g_x11.render_ctx, render_update_callback, nullptr);
    }
    (void)mpv;

    g_platform.shared_texture_supported = false;

    input::x11::init(g_x11.conn, g_x11.screen, g_x11.parent);
    input::x11::set_configure_callback([]() { handle_parent_configure(); });
    input::x11::set_map_callback([](xcb_window_t window, bool mapped) {
        if (window != g_x11.parent) return;
        std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
        set_minimized_locked(!mapped);
    });
    input::x11::set_shutdown_callback([]() {});
    input::x11::start_input_thread();

    g_x11.render_thread = std::thread([]() {
        // Sole owner of compose+present. Coalesces every producer signal
        // (CEF paint, configure, visibility, fade, mpv update) into one
        // pass per wake.
        //
        // surface_mtx is held only during the CEF-layer read pass, so the
        // libmpv SW render and the SHM upload run unlocked — CEF paint
        // threads aren't starved during video playback.
        while (true) {
            {
                std::unique_lock<std::mutex> lock(g_x11.render_mtx);
                g_x11.render_cv.wait(lock, [] {
                    return g_x11.render_requested || g_x11.render_stop;
                });
                if (g_x11.render_stop) return;
                g_x11.render_requested = false;
            }

            // Snapshot size + visibility under lock
            int pw, ph;
            bool minimized;
            {
                std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
                pw = g_x11.pw;
                ph = g_x11.ph;
                minimized = g_x11.minimized;
            }
            if (minimized || pw <= 0 || ph <= 0) continue;
            if (!g_x11.conn || g_x11.parent == XCB_NONE) continue;

            // Phase 1: mpv SW render (self-composite only; overlay mode
            // leaves mpv to render natively into parent).
            if (g_x11.self_composite && g_x11.render_ctx) {
                uint64_t flags = mpv_render_context_update(g_x11.render_ctx);
                if (flags & MPV_RENDER_UPDATE_FRAME)
                    render_mpv_unlocked(pw, ph);
            }

            // Base composition:
            //   self-composite → mpv_layer as the background
            //   overlay mode   → transparent (alpha=0) so mpv shows through
            size_t frame_bytes = static_cast<size_t>(pw) * ph * 4;
            ensure_layer(g_x11.composed_layer, pw, ph, false);
            if (g_x11.self_composite && g_x11.mpv_layer.size() == frame_bytes)
                memcpy(g_x11.composed_layer.data(), g_x11.mpv_layer.data(), frame_bytes);
            else
                std::fill(g_x11.composed_layer.begin(), g_x11.composed_layer.end(), 0);

            // Phase 2: blend CEF layers (lock needed; CEF threads write these).
            bool premul = !g_x11.self_composite;
            {
                std::lock_guard<std::mutex> lock(g_x11.surface_mtx);
                // Abort if the window resized out from under us; next wake redoes.
                if (g_x11.pw != pw || g_x11.ph != ph || g_x11.minimized) continue;
                blend_layer(g_x11.composed_layer, g_x11.main_layer, 255, premul);
                if (g_x11.overlay_visible)
                    blend_layer(g_x11.composed_layer, g_x11.overlay_layer, g_x11.overlay_alpha, premul);
                if (g_x11.about_visible)
                    blend_layer(g_x11.composed_layer, g_x11.about_layer, 255, premul);
            }

            // Phase 3: upload (unlocked; final_buf is render-thread-owned).
            xcb_window_t target = g_x11.self_composite ? g_x11.parent : g_x11.cef_overlay;
            uint8_t depth = g_x11.self_composite ? g_x11.screen->root_depth : g_x11.argb_depth;
            if (target == XCB_NONE) continue;
            if (!shm_alloc(g_x11.final_buf, g_x11.conn, pw, ph)) continue;
            memcpy(g_x11.final_buf.data, g_x11.composed_layer.data(), frame_bytes);
            xcb_shm_put_image(g_x11.conn, target, g_x11.final_gc,
                pw, ph, 0, 0, pw, ph,
                0, 0, depth,
                XCB_IMAGE_FORMAT_Z_PIXMAP,
                0, g_x11.final_buf.seg, 0);
            xcb_flush(g_x11.conn);
        }
    });
    request_render_frame();

    idle_inhibit::init();

    LOG_INFO(LOG_PLATFORM, "X11 platform initialized (parent=0x{:x})", g_x11.parent);
    return true;
}

// =====================================================================
// Cleanup
// =====================================================================

static void x11_cleanup() {
    x11_cancel_fade();

    {
        std::lock_guard<std::mutex> lock(g_x11.render_mtx);
        g_x11.render_stop = true;
        g_x11.render_requested = true;
    }
    g_x11.render_cv.notify_one();
    if (g_x11.render_thread.joinable())
        g_x11.render_thread.join();

    if (g_x11.render_ctx) {
        mpv_render_context_set_update_callback(g_x11.render_ctx, nullptr, nullptr);
        mpv_render_context_free(g_x11.render_ctx);
        g_x11.render_ctx = nullptr;
    }

    // Hide overlay windows immediately so they don't linger during shutdown
    if (g_x11.conn) {
        if (g_x11.cef_overlay != XCB_NONE)
            xcb_unmap_window(g_x11.conn, g_x11.cef_overlay);
        if (g_x11.owns_parent && g_x11.parent != XCB_NONE)
            xcb_unmap_window(g_x11.conn, g_x11.parent);
        xcb_flush(g_x11.conn);
    }

    idle_inhibit::cleanup();
    input::x11::cleanup();

    // Free SHM buffers
    for (auto& buf : g_x11.cef_bufs)     shm_free(buf, g_x11.conn);
    for (auto& buf : g_x11.overlay_bufs)  shm_free(buf, g_x11.conn);
    for (auto& buf : g_x11.about_bufs)    shm_free(buf, g_x11.conn);
    shm_free(g_x11.final_buf, g_x11.conn);

    // Free GCs and destroy windows
    if (g_x11.final_gc != XCB_NONE)
        xcb_free_gc(g_x11.conn, g_x11.final_gc);
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
    if (g_x11.cef_overlay != XCB_NONE)
        xcb_destroy_window(g_x11.conn, g_x11.cef_overlay);
    if (g_x11.owns_parent && g_x11.parent != XCB_NONE)
        xcb_destroy_window(g_x11.conn, g_x11.parent);
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
        .prepare_window = x11_prepare_window,
        .native_window_id = x11_native_window_id,
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
