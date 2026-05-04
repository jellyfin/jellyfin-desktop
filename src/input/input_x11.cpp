#include "input_x11.h"

#include "input.h"
#include "keysym_map.h"
#include "dispatch.h"
#include "../common.h"
#include "../wake_event.h"
#include "logging.h"

#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xcb_event.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>
// xcb/xkb.h uses 'explicit' as a field name — reserved in C++.
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit

#include "include/internal/cef_types.h"

#include <xcb/xcb_cursor.h>

#include <poll.h>
#include <unistd.h>
#include <thread>
#include <cstdint>

extern WakeEvent g_shutdown_event;

namespace input::x11 {
namespace {

struct State {
    xcb_connection_t* conn = nullptr;
    xcb_window_t      window = XCB_NONE;

    std::thread input_thread;

    // XKB keyboard state
    xkb_context* xkb_ctx  = nullptr;
    xkb_keymap*  xkb_kmap = nullptr;
    xkb_state*   xkb_st   = nullptr;
    int32_t      xkb_device_id = -1;
    uint8_t      xkb_base_event = 0;
    uint32_t     modifiers = 0;  // EVENTFLAG_SHIFT/CONTROL/ALT_DOWN bitmask

    // Pointer state
    int      ptr_x = 0, ptr_y = 0;
    uint32_t mouse_button_modifiers = 0;

    // Cursor
    cef_cursor_type_t        cursor_type = CT_POINTER;
    xcb_cursor_t             current_cursor = XCB_NONE;
    xcb_cursor_context_t*    cursor_ctx = nullptr;

    // Configure callback (for overlay repositioning)
    std::function<void()> configure_cb;
    // Shutdown callback (hide overlays before input thread exits)
    std::function<void()> shutdown_cb;
};

State g;

// --- Modifier helpers -------------------------------------------------------

uint32_t xkb_to_cef_mods() {
    uint32_t m = 0;
    if (!g.xkb_st) return m;
    if (xkb_state_mod_name_is_active(g.xkb_st, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE)) m |= EVENTFLAG_SHIFT_DOWN;
    if (xkb_state_mod_name_is_active(g.xkb_st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE))  m |= EVENTFLAG_CONTROL_DOWN;
    if (xkb_state_mod_name_is_active(g.xkb_st, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE))   m |= EVENTFLAG_ALT_DOWN;
    return m;
}

uint32_t cef_modifiers() {
    return g.modifiers | g.mouse_button_modifiers;
}

using input::keysym_to_keycode;
using input::keysym_to_vkey;

// --- XKB setup --------------------------------------------------------------

bool setup_xkb() {
    if (!xkb_x11_setup_xkb_extension(g.conn,
            XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
            XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
            nullptr, nullptr, &g.xkb_base_event, nullptr))
        return false;

    g.xkb_device_id = xkb_x11_get_core_keyboard_device_id(g.conn);
    if (g.xkb_device_id < 0) return false;

    g.xkb_kmap = xkb_x11_keymap_new_from_device(g.xkb_ctx, g.conn,
        g.xkb_device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!g.xkb_kmap) return false;

    g.xkb_st = xkb_x11_state_new_from_device(g.xkb_kmap, g.conn, g.xkb_device_id);
    if (!g.xkb_st) return false;

    // Subscribe to XKB state notify events for modifier tracking
    uint16_t required_map_parts =
        XCB_XKB_MAP_PART_KEY_TYPES |
        XCB_XKB_MAP_PART_KEY_SYMS |
        XCB_XKB_MAP_PART_MODIFIER_MAP |
        XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
        XCB_XKB_MAP_PART_KEY_ACTIONS |
        XCB_XKB_MAP_PART_VIRTUAL_MODS |
        XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP;
    uint16_t required_events =
        XCB_XKB_EVENT_TYPE_STATE_NOTIFY |
        XCB_XKB_EVENT_TYPE_MAP_NOTIFY |
        XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY;

    xcb_xkb_select_events(g.conn, g.xkb_device_id,
        required_events, 0, required_events,
        required_map_parts, required_map_parts, nullptr);

    return true;
}

void update_xkb_keymap() {
    if (g.xkb_st)   xkb_state_unref(g.xkb_st);
    if (g.xkb_kmap) xkb_keymap_unref(g.xkb_kmap);
    g.xkb_kmap = xkb_x11_keymap_new_from_device(g.xkb_ctx, g.conn,
        g.xkb_device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (g.xkb_kmap)
        g.xkb_st = xkb_x11_state_new_from_device(g.xkb_kmap, g.conn, g.xkb_device_id);
}

// --- Event handlers ---------------------------------------------------------

void handle_key(xcb_key_press_event_t* ev, bool pressed) {
    if (!g.xkb_st) return;
    xkb_keycode_t kc = ev->detail;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(g.xkb_st, kc);

    KeyEvent e{};
    e.code             = keysym_to_keycode(sym);
    e.windows_key_code = keysym_to_vkey(sym);
    e.action           = pressed ? KeyAction::Down : KeyAction::Up;
    e.modifiers        = g.modifiers;
    e.native_key_code  = static_cast<int>(kc) - 8;  // X keycode to Linux input code
    e.is_system_key    = false;
    input::dispatch_key(e);

    if (pressed) {
        uint32_t cp = xkb_state_key_get_utf32(g.xkb_st, kc);
        if (cp > 0)
            input::dispatch_char(cp, g.modifiers, e.native_key_code, false);
    }

    // Update XKB state for modifier changes
    xkb_state_update_key(g.xkb_st, kc,
        pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    g.modifiers = xkb_to_cef_mods();
}

// X11 pointer coordinates are physical pixels. CEF expects logical.
int to_logical(int physical) {
    float scale = g_platform.get_scale();
    return static_cast<int>(physical / scale);
}

void handle_button(xcb_button_press_event_t* ev, bool pressed) {
    uint32_t button = ev->detail;
    int x = to_logical(ev->event_x);
    int y = to_logical(ev->event_y);
    LOG_TRACE(LOG_PLATFORM, "[INPUT] xcb_button code={} pressed={}", button, pressed);

    // Buttons 4-7 are scroll wheel events on X11
    if (button >= 4 && button <= 7) {
        if (!pressed) return;  // only handle press for scroll
        int dx = 0, dy = 0;
        switch (button) {
        case 4: dy = 120;  break;  // scroll up
        case 5: dy = -120; break;  // scroll down
        case 6: dx = 120;  break;  // scroll left
        case 7: dx = -120; break;  // scroll right
        }
        input::dispatch_scroll({
            .x = x, .y = y,
            .dx = dx, .dy = dy,
            .modifiers = cef_modifiers(),
        });
        return;
    }

    // X11 mouse buttons 8/9 are "back"/"forward" side buttons.
    constexpr uint32_t XCB_BUTTON_BACK    = 8;
    constexpr uint32_t XCB_BUTTON_FORWARD = 9;
    if (button == XCB_BUTTON_BACK || button == XCB_BUTTON_FORWARD) {
        if (pressed) input::dispatch_history_nav(button == XCB_BUTTON_FORWARD);
        return;
    }

    MouseButton btn;
    uint32_t flag;
    switch (button) {
    case 1: btn = MouseButton::Left;   flag = EVENTFLAG_LEFT_MOUSE_BUTTON;   break;
    case 2: btn = MouseButton::Middle; flag = EVENTFLAG_MIDDLE_MOUSE_BUTTON; break;
    case 3: btn = MouseButton::Right;  flag = EVENTFLAG_RIGHT_MOUSE_BUTTON;  break;
    default: return;
    }

    if (pressed) g.mouse_button_modifiers |= flag;
    else         g.mouse_button_modifiers &= ~flag;

    input::dispatch_mouse_button({
        .button = btn,
        .pressed = pressed,
        .x = x, .y = y,
        .click_count = 1,
        .modifiers = cef_modifiers(),
    });
}

void handle_motion(xcb_motion_notify_event_t* ev) {
    g.ptr_x = to_logical(ev->event_x);
    g.ptr_y = to_logical(ev->event_y);
    input::dispatch_mouse_move({
        .x = g.ptr_x, .y = g.ptr_y,
        .modifiers = cef_modifiers(),
        .leave = false,
    });
}

void handle_enter(xcb_enter_notify_event_t* ev) {
    g.ptr_x = to_logical(ev->event_x);
    g.ptr_y = to_logical(ev->event_y);
    set_cursor(g.cursor_type);
    input::dispatch_mouse_move({
        .x = g.ptr_x, .y = g.ptr_y,
        .modifiers = cef_modifiers(),
        .leave = false,
    });
}

void handle_leave(xcb_leave_notify_event_t*) {
    input::dispatch_mouse_move({
        .x = g.ptr_x, .y = g.ptr_y,
        .modifiers = cef_modifiers(),
        .leave = true,
    });
}


void handle_xkb_event(xcb_generic_event_t* ev) {
    uint8_t xkb_type = ev->pad0;
    if (xkb_type == XCB_XKB_STATE_NOTIFY) {
        auto* state_ev = reinterpret_cast<xcb_xkb_state_notify_event_t*>(ev);
        xkb_state_update_mask(g.xkb_st,
            state_ev->baseMods, state_ev->latchedMods, state_ev->lockedMods,
            state_ev->baseGroup, state_ev->latchedGroup, state_ev->lockedGroup);
        g.modifiers = xkb_to_cef_mods();
    } else if (xkb_type == XCB_XKB_MAP_NOTIFY ||
               xkb_type == XCB_XKB_NEW_KEYBOARD_NOTIFY) {
        update_xkb_keymap();
    }
}

// --- Cursor shape translation -----------------------------------------------

const char* cef_cursor_to_name(cef_cursor_type_t type) {
    switch (type) {
    case CT_CROSS:                      return "crosshair";
    case CT_HAND:                       return "pointer";
    case CT_IBEAM:                      return "text";
    case CT_WAIT:                       return "wait";
    case CT_HELP:                       return "help";
    case CT_EASTRESIZE:                 return "e-resize";
    case CT_NORTHRESIZE:                return "n-resize";
    case CT_NORTHEASTRESIZE:            return "ne-resize";
    case CT_NORTHWESTRESIZE:            return "nw-resize";
    case CT_SOUTHRESIZE:                return "s-resize";
    case CT_SOUTHEASTRESIZE:            return "se-resize";
    case CT_SOUTHWESTRESIZE:            return "sw-resize";
    case CT_WESTRESIZE:                 return "w-resize";
    case CT_NORTHSOUTHRESIZE:           return "ns-resize";
    case CT_EASTWESTRESIZE:             return "ew-resize";
    case CT_NORTHEASTSOUTHWESTRESIZE:   return "nesw-resize";
    case CT_NORTHWESTSOUTHEASTRESIZE:   return "nwse-resize";
    case CT_COLUMNRESIZE:               return "col-resize";
    case CT_ROWRESIZE:                  return "row-resize";
    case CT_MOVE:                       return "move";
    case CT_VERTICALTEXT:               return "vertical-text";
    case CT_CELL:                       return "cell";
    case CT_CONTEXTMENU:                return "context-menu";
    case CT_ALIAS:                      return "alias";
    case CT_PROGRESS:                   return "progress";
    case CT_NODROP:                     return "no-drop";
    case CT_NOTALLOWED:                 return "not-allowed";
    case CT_ZOOMIN:                     return "zoom-in";
    case CT_ZOOMOUT:                    return "zoom-out";
    case CT_GRAB:                       return "grab";
    case CT_GRABBING:                   return "grabbing";
    case CT_MIDDLEPANNING:
    case CT_MIDDLE_PANNING_VERTICAL:
    case CT_MIDDLE_PANNING_HORIZONTAL:  return "all-scroll";
    default:                            return "default";
    }
}

// --- Input thread -----------------------------------------------------------

void input_thread_func() {
    int xcb_fd = xcb_get_file_descriptor(g.conn);
    struct pollfd fds[2] = {
        {xcb_fd, POLLIN, 0},
        {g_shutdown_event.fd(), POLLIN, 0},
    };

    while (true) {
        xcb_flush(g.conn);
        poll(fds, 2, -1);

        if (fds[1].revents & POLLIN) {
            if (g.shutdown_cb) g.shutdown_cb();
            break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (g.shutdown_cb) g.shutdown_cb();
            break;
        }

        xcb_generic_event_t* ev;
        while ((ev = xcb_poll_for_event(g.conn)) != nullptr) {
            uint8_t type = XCB_EVENT_RESPONSE_TYPE(ev);

            if (type == g.xkb_base_event) {
                handle_xkb_event(ev);
                free(ev);
                continue;
            }

            switch (type) {
            case XCB_KEY_PRESS:
                handle_key(reinterpret_cast<xcb_key_press_event_t*>(ev), true);
                break;
            case XCB_KEY_RELEASE:
                handle_key(reinterpret_cast<xcb_key_release_event_t*>(ev), false);
                break;
            case XCB_BUTTON_PRESS:
                handle_button(reinterpret_cast<xcb_button_press_event_t*>(ev), true);
                break;
            case XCB_BUTTON_RELEASE:
                handle_button(reinterpret_cast<xcb_button_release_event_t*>(ev), false);
                break;
            case XCB_MOTION_NOTIFY:
                handle_motion(reinterpret_cast<xcb_motion_notify_event_t*>(ev));
                break;
            case XCB_ENTER_NOTIFY:
                handle_enter(reinterpret_cast<xcb_enter_notify_event_t*>(ev));
                break;
            case XCB_LEAVE_NOTIFY:
                handle_leave(reinterpret_cast<xcb_leave_notify_event_t*>(ev));
                break;
            case XCB_CONFIGURE_NOTIFY:
                if (g.configure_cb) g.configure_cb();
                break;
            case XCB_DESTROY_NOTIFY:
                // mpv's window was destroyed — shut down
                initiate_shutdown();
                break;
            case XCB_CLIENT_MESSAGE:
                // WM_DELETE_WINDOW on our overlay windows (WM targets
                // the focused window, which may be our overlay)
                initiate_shutdown();
                break;
            }
            free(ev);
        }
    }
}

}  // namespace

// --- Public API -------------------------------------------------------------

void init(xcb_connection_t* conn, xcb_screen_t* screen, xcb_window_t window) {
    g.conn = conn;
    g.window = window;
    g.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    setup_xkb();

    xcb_cursor_context_new(conn, screen, &g.cursor_ctx);

    // Select input + structure events on the window.
    // StructureNotify delivers ConfigureNotify for overlay repositioning.
    uint32_t mask = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
                    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                    XCB_EVENT_MASK_POINTER_MOTION |
                    XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn, window, XCB_CW_EVENT_MASK, &mask);
    xcb_flush(conn);
}

void set_configure_callback(std::function<void()> cb) {
    g.configure_cb = std::move(cb);
}

void set_shutdown_callback(std::function<void()> cb) {
    g.shutdown_cb = std::move(cb);
}

void start_input_thread() {
    g.input_thread = std::thread(input_thread_func);
}

void cleanup() {
    if (g.input_thread.joinable()) g.input_thread.join();
    if (g.current_cursor != XCB_NONE && g.conn) {
        xcb_free_cursor(g.conn, g.current_cursor);
        g.current_cursor = XCB_NONE;
    }
    if (g.cursor_ctx) {
        xcb_cursor_context_free(g.cursor_ctx);
        g.cursor_ctx = nullptr;
    }
    if (g.xkb_st)   { xkb_state_unref(g.xkb_st);     g.xkb_st = nullptr; }
    if (g.xkb_kmap) { xkb_keymap_unref(g.xkb_kmap);   g.xkb_kmap = nullptr; }
    if (g.xkb_ctx)  { xkb_context_unref(g.xkb_ctx);   g.xkb_ctx = nullptr; }
}

void set_cursor(cef_cursor_type_t type) {
    g.cursor_type = type;
    if (!g.conn || g.window == XCB_NONE) return;

    if (type == CT_NONE) {
        // Create invisible 1x1 cursor
        xcb_pixmap_t pix = xcb_generate_id(g.conn);
        xcb_create_pixmap(g.conn, 1, pix, g.window, 1, 1);
        xcb_cursor_t blank = xcb_generate_id(g.conn);
        xcb_create_cursor(g.conn, blank, pix, pix, 0, 0, 0, 0, 0, 0, 0, 0);
        uint32_t val = blank;
        xcb_change_window_attributes(g.conn, g.window, XCB_CW_CURSOR, &val);
        xcb_flush(g.conn);
        if (g.current_cursor != XCB_NONE)
            xcb_free_cursor(g.conn, g.current_cursor);
        g.current_cursor = blank;
        xcb_free_pixmap(g.conn, pix);
        return;
    }

    if (!g.cursor_ctx) return;

    xcb_cursor_t cursor = xcb_cursor_load_cursor(g.cursor_ctx, cef_cursor_to_name(type));
    if (cursor == XCB_CURSOR_NONE) return;

    uint32_t val = cursor;
    xcb_change_window_attributes(g.conn, g.window, XCB_CW_CURSOR, &val);
    xcb_flush(g.conn);

    if (g.current_cursor != XCB_NONE)
        xcb_free_cursor(g.conn, g.current_cursor);
    g.current_cursor = cursor;
}

}  // namespace input::x11
