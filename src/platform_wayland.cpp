#include "common.h"
#include "cef/cef_client.h"

#include <wayland-client.h>
#include "linux-dmabuf-v1-client.h"
#include "viewporter-client.h"
#include "alpha-modifier-v1-client.h"
// Callback fields in mpv's vo_wayland_state -- set via wayland-state property.
// Must match the struct layout in wayland_common.h.
struct wl_configure_cb {
    void (*fn)(void *data, int width, int height, bool fullscreen);
    void *data;
};
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <drm/drm_fourcc.h>
#include <EGL/egl.h>
#ifdef HAVE_KDE_DECORATION_PALETTE
#include "server-decoration-palette-client.h"
#endif
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <mutex>
#include <thread>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "wake_event.h"
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

    // Shared globals
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

    // Input
    std::thread input_thread;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    double ptr_x = 0, ptr_y = 0;
    xkb_context* xkb_ctx = nullptr;
    xkb_keymap* xkb_kmap = nullptr;
    xkb_state* xkb_st = nullptr;
    uint32_t modifiers = 0;

#ifdef HAVE_KDE_DECORATION_PALETTE
    org_kde_kwin_server_decoration_palette_manager* palette_manager = nullptr;
    org_kde_kwin_server_decoration_palette* palette = nullptr;
    std::string colors_dir;
    std::string colors_path;
#endif
};

static WlState g_wl;

static void input_thread_func();
static void update_surface_size_locked(int lw, int lh, int pw, int ph);
static void wl_begin_transition_locked();
static void wl_end_transition_locked();
static void wl_begin_transition();
static void wl_toggle_fullscreen();
static void wl_init_kde_palette();
static void wl_cleanup_kde_palette();
static void wl_set_titlebar_color(uint8_t r, uint8_t g, uint8_t b);

// Helper: get the active browser for input routing
static CefRefPtr<CefBrowser> active_browser() {
    if (g_wl.overlay_visible && g_overlay_client && g_overlay_client->browser())
        return g_overlay_client->browser();
    if (g_client && g_client->browser())
        return g_client->browser();
    return nullptr;
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

static void wl_overlay_resize(int lw, int lh, int pw, int ph) {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (!g_wl.overlay_surface || !g_wl.overlay_viewport) return;
    wp_viewport_set_source(g_wl.overlay_viewport,
        wl_fixed_from_int(0), wl_fixed_from_int(0),
        wl_fixed_from_int(pw), wl_fixed_from_int(ph));
    wp_viewport_set_destination(g_wl.overlay_viewport, lw, lh);
    wl_surface_commit(g_wl.overlay_surface);
    wl_display_flush(g_wl.display);
}

static void wl_set_overlay_visible(bool visible) {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (g_wl.overlay_visible == visible) return;
    g_wl.overlay_visible = visible;
    if (!g_wl.overlay_surface) return;
    if (!visible) {
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
    }
}

// Animate overlay alpha from opaque to transparent over duration_sec, then hide.
// Runs on a detached thread — this is a finite UI animation, not a poll loop.
static void wl_fade_overlay(float duration_sec) {
    if (!g_wl.overlay_alpha || !g_wl.overlay_surface) {
        // No alpha modifier support — just hide immediately
        wl_set_overlay_visible(false);
        return;
    }

    std::thread([duration_sec]() {
        constexpr int fps = 60;
        int total_frames = static_cast<int>(duration_sec * fps);
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
    }).detach();
}

// =====================================================================
// Input: wl_pointer + wl_keyboard -> CEF
// =====================================================================

static uint32_t xkb_to_cef_mods() {
    uint32_t m = 0;
    if (!g_wl.xkb_st) return m;
    if (xkb_state_mod_name_is_active(g_wl.xkb_st, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE)) m |= EVENTFLAG_SHIFT_DOWN;
    if (xkb_state_mod_name_is_active(g_wl.xkb_st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE))  m |= EVENTFLAG_CONTROL_DOWN;
    if (xkb_state_mod_name_is_active(g_wl.xkb_st, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE))   m |= EVENTFLAG_ALT_DOWN;
    return m;
}

static int keysym_to_vkey(xkb_keysym_t sym) {
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) return 'A' + (sym - XKB_KEY_a);
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) return sym;
    if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) return sym;
    switch (sym) {
    case XKB_KEY_Return: return 0x0D; case XKB_KEY_Escape: return 0x1B;
    case XKB_KEY_Tab: return 0x09; case XKB_KEY_BackSpace: return 0x08;
    case XKB_KEY_space: return 0x20;
    case XKB_KEY_Left: return 0x25; case XKB_KEY_Up: return 0x26;
    case XKB_KEY_Right: return 0x27; case XKB_KEY_Down: return 0x28;
    case XKB_KEY_Home: return 0x24; case XKB_KEY_End: return 0x23;
    case XKB_KEY_Page_Up: return 0x21; case XKB_KEY_Page_Down: return 0x22;
    case XKB_KEY_Delete: return 0x2E;
    case XKB_KEY_F1: case XKB_KEY_F2: case XKB_KEY_F3: case XKB_KEY_F4:
    case XKB_KEY_F5: case XKB_KEY_F6: case XKB_KEY_F7: case XKB_KEY_F8:
    case XKB_KEY_F9: case XKB_KEY_F10: case XKB_KEY_F11: case XKB_KEY_F12:
        return 0x70 + (sym - XKB_KEY_F1);
    default: return 0;
    }
}

// Pointer
static void ptr_enter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
    g_wl.ptr_x = wl_fixed_to_double(x); g_wl.ptr_y = wl_fixed_to_double(y);
    auto b = active_browser();
    if (!b) return;
    CefMouseEvent e; e.x = (int)g_wl.ptr_x; e.y = (int)g_wl.ptr_y; e.modifiers = g_wl.modifiers;
    b->GetHost()->SendMouseMoveEvent(e, false);
}
static void ptr_leave(void*, wl_pointer*, uint32_t, wl_surface*) {
    auto b = active_browser();
    if (!b) return;
    CefMouseEvent e; e.x = (int)g_wl.ptr_x; e.y = (int)g_wl.ptr_y; e.modifiers = g_wl.modifiers;
    b->GetHost()->SendMouseMoveEvent(e, true);
}
static void ptr_motion(void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    g_wl.ptr_x = wl_fixed_to_double(x); g_wl.ptr_y = wl_fixed_to_double(y);
    auto b = active_browser();
    if (!b) return;
    CefMouseEvent e; e.x = (int)g_wl.ptr_x; e.y = (int)g_wl.ptr_y; e.modifiers = g_wl.modifiers;
    b->GetHost()->SendMouseMoveEvent(e, false);
}
static void ptr_button(void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    auto b = active_browser();
    if (!b) return;
    cef_mouse_button_type_t btn;
    switch (button) {
    case BTN_LEFT: btn = MBT_LEFT; break;
    case BTN_RIGHT: btn = MBT_RIGHT; break;
    case BTN_MIDDLE: btn = MBT_MIDDLE; break;
    default: return;
    }
    CefMouseEvent e; e.x = (int)g_wl.ptr_x; e.y = (int)g_wl.ptr_y; e.modifiers = g_wl.modifiers;
    b->GetHost()->SendMouseClickEvent(e, btn, state == WL_POINTER_BUTTON_STATE_RELEASED, 1);
}
static void ptr_axis(void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    auto b = active_browser();
    if (!b) return;
    CefMouseEvent e; e.x = (int)g_wl.ptr_x; e.y = (int)g_wl.ptr_y; e.modifiers = g_wl.modifiers;
    int d = -wl_fixed_to_int(value) * 3;
    if (axis == 0) b->GetHost()->SendMouseWheelEvent(e, 0, d);
    else b->GetHost()->SendMouseWheelEvent(e, d, 0);
}
static void ptr_frame(void*, wl_pointer*) {}
static void ptr_axis_source(void*, wl_pointer*, uint32_t) {}
static void ptr_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
static void ptr_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
static void ptr_axis_value120(void*, wl_pointer*, uint32_t, int32_t) {}
static void ptr_axis_relative(void*, wl_pointer*, uint32_t, uint32_t) {}

static const wl_pointer_listener s_ptr = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
    .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete, .axis_value120 = ptr_axis_value120,
    .axis_relative_direction = ptr_axis_relative,
};

// Keyboard
static void kb_keymap(void*, wl_keyboard*, uint32_t fmt, int fd, uint32_t size) {
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (map == MAP_FAILED) return;
    if (g_wl.xkb_st) xkb_state_unref(g_wl.xkb_st);
    if (g_wl.xkb_kmap) xkb_keymap_unref(g_wl.xkb_kmap);
    g_wl.xkb_kmap = xkb_keymap_new_from_buffer(g_wl.xkb_ctx, map, size - 1,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (g_wl.xkb_kmap) g_wl.xkb_st = xkb_state_new(g_wl.xkb_kmap);
}
static void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
    auto b = active_browser();
    if (b) b->GetHost()->SetFocus(true);
}
static void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {
    auto b = active_browser();
    if (b) b->GetHost()->SetFocus(false);
}
static void kb_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    if (!g_wl.xkb_st) return;
    uint32_t kc = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(g_wl.xkb_st, kc);
    uint32_t cp = xkb_state_key_get_utf32(g_wl.xkb_st, kc);
    int vk = keysym_to_vkey(sym);

    // Only handle hotkeys when overlay is not visible
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && !g_wl.overlay_visible) {
        // Fullscreen: f or F11
        if (sym == XKB_KEY_f || sym == XKB_KEY_F11) {
            wl_toggle_fullscreen();
            return;
        }
        // Quit: q or Escape
        if (sym == XKB_KEY_q || sym == XKB_KEY_Escape) {
            initiate_shutdown();
            return;
        }
    }

    // Forward to active browser
    auto b = active_browser();
    if (!b) return;
    CefKeyEvent ev;
    ev.windows_key_code = vk;
    ev.native_key_code = key;
    ev.modifiers = g_wl.modifiers;
    ev.is_system_key = false;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        ev.type = KEYEVENT_RAWKEYDOWN;
        b->GetHost()->SendKeyEvent(ev);
        if (cp > 0 && cp < 0x10FFFF) {
            ev.type = KEYEVENT_CHAR;
            ev.character = cp;
            ev.windows_key_code = cp;
            b->GetHost()->SendKeyEvent(ev);
        }
    } else {
        ev.type = KEYEVENT_KEYUP;
        b->GetHost()->SendKeyEvent(ev);
    }
}
static void kb_modifiers(void*, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
    if (g_wl.xkb_st) {
        xkb_state_update_mask(g_wl.xkb_st, dep, lat, lock, 0, 0, grp);
        g_wl.modifiers = xkb_to_cef_mods();
    }
}
static void kb_repeat(void*, wl_keyboard*, int32_t, int32_t) {}

static const wl_keyboard_listener s_kb = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
    .key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat,
};

// Seat
static void seat_caps(void*, wl_seat* seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_wl.pointer) {
        g_wl.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_wl.pointer, &s_ptr, nullptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_wl.keyboard) {
        g_wl.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_wl.keyboard, &s_kb, nullptr);
    }
}
static void seat_name(void*, wl_seat*, const char*) {}
static const wl_seat_listener s_seat = { .capabilities = seat_caps, .name = seat_name };

// =====================================================================
// Registry
// =====================================================================

static void reg_global(void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        g_wl.compositor = static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    else if (strcmp(iface, wl_subcompositor_interface.name) == 0)
        g_wl.subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(reg, name, &wl_subcompositor_interface, 1));
    else if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0)
        g_wl.dmabuf = static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, std::min(ver, 4u)));
    else if (strcmp(iface, wp_viewporter_interface.name) == 0)
        g_wl.viewporter = static_cast<wp_viewporter*>(wl_registry_bind(reg, name, &wp_viewporter_interface, 1));
    else if (strcmp(iface, wp_alpha_modifier_v1_interface.name) == 0)
        g_wl.alpha_modifier = static_cast<wp_alpha_modifier_v1*>(wl_registry_bind(reg, name, &wp_alpha_modifier_v1_interface, 1));
    else if (strcmp(iface, wl_seat_interface.name) == 0 && !g_wl.seat) {
        g_wl.seat = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 5u)));
        wl_seat_add_listener(g_wl.seat, &s_seat, nullptr);
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
        if (!g_wl.transitioning)
            wl_begin_transition_locked();
        else
            wl_end_transition_locked();
        g_wl.was_fullscreen = fs;
    }

    update_surface_size_locked(lw, lh, pw, ph);
}

// =====================================================================
// Platform interface
// =====================================================================

static bool wl_init(mpv_handle* mpv) {
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
    g_wl.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    // Dedicated event queue: all our objects live here, isolated from mpv's VO queue
    g_wl.queue = wl_display_create_queue(display);

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

    wl_display_roundtrip_queue(display, g_wl.queue);

    // Register mpv configure callback
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

    // EGL init for CEF shared texture support
    EGLDisplay egl_dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(g_wl.display));
    if (egl_dpy != EGL_NO_DISPLAY) eglInitialize(egl_dpy, nullptr, nullptr);

    // KDE titlebar color — #101010 matches the loading screen background
    wl_init_kde_palette();
    wl_set_titlebar_color(0x10, 0x10, 0x10);

    // Start input thread
    g_wl.input_thread = std::thread(input_thread_func);

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
    if (g_wl.input_thread.joinable()) g_wl.input_thread.join();
    if (g_wl.pointer) wl_pointer_destroy(g_wl.pointer);
    if (g_wl.keyboard) wl_keyboard_destroy(g_wl.keyboard);
    if (g_wl.seat) wl_seat_destroy(g_wl.seat);
    if (g_wl.xkb_st) xkb_state_unref(g_wl.xkb_st);
    if (g_wl.xkb_kmap) xkb_keymap_unref(g_wl.xkb_kmap);
    if (g_wl.xkb_ctx) xkb_context_unref(g_wl.xkb_ctx);
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
    // Globals (must be destroyed before queue — they were bound to it)
    if (g_wl.alpha_modifier) { wp_alpha_modifier_v1_destroy(g_wl.alpha_modifier); g_wl.alpha_modifier = nullptr; }
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
        if (current == fullscreen) return;  // already in desired state
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

static void wl_set_expected_size(int w, int h) {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    if (g_wl.transitioning && w == g_wl.transition_pw && h == g_wl.transition_ph)
        return;
    g_wl.expected_w = w;
    g_wl.expected_h = h;
}

static void wl_end_transition() {
    std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
    wl_end_transition_locked();
}

static void input_thread_func() {
    int display_fd = wl_display_get_fd(g_wl.display);
    struct pollfd fds[2] = {
        {display_fd, POLLIN, 0},
        {g_shutdown_event.fd(), POLLIN, 0},
    };
    while (true) {
        while (wl_display_prepare_read_queue(g_wl.display, g_wl.queue) != 0)
            wl_display_dispatch_queue_pending(g_wl.display, g_wl.queue);
        wl_display_flush(g_wl.display);

        poll(fds, 2, -1);

        if (fds[0].revents & POLLIN) {
            wl_display_read_events(g_wl.display);
        } else {
            wl_display_cancel_read(g_wl.display);
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (fds[1].revents & POLLIN)
            break;

        wl_display_dispatch_queue_pending(g_wl.display, g_wl.queue);
    }
}

static void wl_pump() {}

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
    g_wl.palette_manager = nullptr;
    if (!g_wl.colors_path.empty()) {
        remove(g_wl.colors_path.c_str());
        g_wl.colors_path.clear();
    }
}

static void wl_set_titlebar_color(uint8_t r, uint8_t g, uint8_t b) {
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
        .early_init = []() {},
        .init = wl_init,
        .cleanup = wl_cleanup,
        .present = wl_present,
        .present_software = nullptr,
        .resize = wl_resize,
        .overlay_present = wl_overlay_present,
        .overlay_present_software = nullptr,
        .overlay_resize = wl_overlay_resize,
        .set_overlay_visible = wl_set_overlay_visible,
        .fade_overlay = wl_fade_overlay,
        .set_fullscreen = wl_set_fullscreen,
        .toggle_fullscreen = wl_toggle_fullscreen,
        .begin_transition = wl_begin_transition,
        .end_transition = wl_end_transition,
        .in_transition = wl_in_transition,
        .set_expected_size = wl_set_expected_size,
        .get_scale = wl_get_scale,
        .query_logical_content_size = [](int*, int*) -> bool { return false; },
        .pump = wl_pump,
        .set_titlebar_color = wl_set_titlebar_color,
    };
}
