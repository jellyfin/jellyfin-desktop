#include "input_wayland.h"

#include "input.h"
#include "keysym_map.h"
#include "dispatch.h"
#include "../common.h"
#include "../wake_event.h"
#include "logging.h"

#include <wayland-client.h>
#include "cursor-shape-v1-client.h"
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

#include "include/internal/cef_types.h"

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <cstdint>

extern WakeEvent g_shutdown_event;

namespace input::wayland {
namespace {

struct State {
    // Borrowed display + queue. Owned by platform_wayland.
    wl_display*      display = nullptr;
    wl_event_queue*  queue   = nullptr;

    // Input thread polling wl_display's fd.
    std::thread input_thread;

    // Seat and devices.
    wl_seat*     seat     = nullptr;
    wl_pointer*  pointer  = nullptr;
    wl_keyboard* keyboard = nullptr;

    // Cursor state. cursor_shape_manager is borrowed (bound in
    // platform_wayland's registry), the device is owned by input.
    wp_cursor_shape_manager_v1* cursor_shape_manager = nullptr;
    wp_cursor_shape_device_v1*  cursor_shape_device  = nullptr;
    cef_cursor_type_t           cursor_type          = CT_POINTER;

    // Pointer state.
    double   ptr_x = 0, ptr_y = 0;
    uint32_t pointer_serial = 0;
    uint32_t mouse_button_modifiers = 0;  // EVENTFLAG_*_MOUSE_BUTTON

    // Scroll accumulation across a single pointer frame.
    double scroll_dx = 0, scroll_dy = 0;      // smooth/touchpad (surface px)
    int    scroll_v120_x = 0, scroll_v120_y = 0;
    bool   scroll_have_v120 = false;

    // XKB state for keyboard translation.
    xkb_context* xkb_ctx  = nullptr;
    xkb_keymap*  xkb_kmap = nullptr;
    xkb_state*   xkb_st   = nullptr;
    uint32_t     modifiers = 0;  // EVENTFLAG_SHIFT/CONTROL/ALT_DOWN bitmask
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

// --- Pointer listener -------------------------------------------------------

void ptr_enter(void*, wl_pointer*, uint32_t serial, wl_surface*,
               wl_fixed_t x, wl_fixed_t y) {
    g.pointer_serial = serial;
    // Reapply stored cursor with the fresh serial.
    set_cursor(g.cursor_type);
    g.ptr_x = wl_fixed_to_double(x);
    g.ptr_y = wl_fixed_to_double(y);
    input::dispatch_mouse_move({
        .x = (int)g.ptr_x, .y = (int)g.ptr_y,
        .modifiers = cef_modifiers(),
        .leave = false,
    });
}

void ptr_leave(void*, wl_pointer*, uint32_t, wl_surface*) {
    input::dispatch_mouse_move({
        .x = (int)g.ptr_x, .y = (int)g.ptr_y,
        .modifiers = cef_modifiers(),
        .leave = true,
    });
}

void ptr_motion(void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    g.ptr_x = wl_fixed_to_double(x);
    g.ptr_y = wl_fixed_to_double(y);
    input::dispatch_mouse_move({
        .x = (int)g.ptr_x, .y = (int)g.ptr_y,
        .modifiers = cef_modifiers(),
        .leave = false,
    });
}

void ptr_button(void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    const bool pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    LOG_TRACE(LOG_PLATFORM, "[INPUT] ptr_button code=0x{:x} pressed={}", button, pressed);
    if (button == BTN_SIDE || button == BTN_EXTRA ||
        button == BTN_BACK || button == BTN_FORWARD) {
        const bool forward = (button == BTN_EXTRA || button == BTN_FORWARD);
        if (pressed) input::dispatch_history_nav(forward);
        return;
    }
    MouseButton btn;
    uint32_t flag;
    switch (button) {
    case BTN_LEFT:   btn = MouseButton::Left;   flag = EVENTFLAG_LEFT_MOUSE_BUTTON;   break;
    case BTN_RIGHT:  btn = MouseButton::Right;  flag = EVENTFLAG_RIGHT_MOUSE_BUTTON;  break;
    case BTN_MIDDLE: btn = MouseButton::Middle; flag = EVENTFLAG_MIDDLE_MOUSE_BUTTON; break;
    default: return;
    }
    if (pressed) g.mouse_button_modifiers |= flag;
    else         g.mouse_button_modifiers &= ~flag;

    input::dispatch_mouse_button({
        .button = btn,
        .pressed = pressed,
        .x = (int)g.ptr_x, .y = (int)g.ptr_y,
        .click_count = 1,
        .modifiers = cef_modifiers(),
    });
}

void ptr_axis(void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    double v = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) g.scroll_dy += v;
    else                                         g.scroll_dx += v;
}

void ptr_frame(void*, wl_pointer*) {
    int dx = 0, dy = 0;
    if (g.scroll_have_v120) {
        // axis_value120: 120 units per notch, matches CEF convention.
        dx = -g.scroll_v120_x;
        dy = -g.scroll_v120_y;
        g.scroll_dx = g.scroll_dy = 0;  // discard any smooth remainder
    } else if (g.scroll_dx != 0.0 || g.scroll_dy != 0.0) {
        // Smooth scroll (touchpad): surface-local pixels → CEF units.
        // ~10px per notch typical, CEF expects 120 per notch → ×12.
        double scaled_x = -g.scroll_dx * 12.0;
        double scaled_y = -g.scroll_dy * 12.0;
        dx = (int)scaled_x;
        dy = (int)scaled_y;
        // Keep fractional remainder so sub-pixel deltas accumulate
        // across frames instead of silently truncating.
        g.scroll_dx = -(scaled_x - dx) / 12.0;
        g.scroll_dy = -(scaled_y - dy) / 12.0;
    } else {
        g.scroll_dx = g.scroll_dy = 0;
    }
    g.scroll_v120_x = g.scroll_v120_y = 0;
    g.scroll_have_v120 = false;
    if (dx == 0 && dy == 0) return;

    input::dispatch_scroll({
        .x = (int)g.ptr_x, .y = (int)g.ptr_y,
        .dx = dx, .dy = dy,
        .modifiers = cef_modifiers(),
    });
}

void ptr_axis_source(void*, wl_pointer*, uint32_t) {}

void ptr_axis_stop(void*, wl_pointer*, uint32_t, uint32_t axis) {
    // Finger lifted from touchpad — drop fractional remainder to
    // prevent ghost scroll.
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) g.scroll_dy = 0;
    else                                         g.scroll_dx = 0;
}

void ptr_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}

void ptr_axis_value120(void*, wl_pointer*, uint32_t axis, int32_t v120) {
    g.scroll_have_v120 = true;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) g.scroll_v120_y += v120;
    else                                         g.scroll_v120_x += v120;
}

void ptr_axis_relative(void*, wl_pointer*, uint32_t, uint32_t) {}

const wl_pointer_listener s_ptr = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
    .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete, .axis_value120 = ptr_axis_value120,
    .axis_relative_direction = ptr_axis_relative,
};

// --- Keyboard listener ------------------------------------------------------

void kb_keymap(void*, wl_keyboard*, uint32_t fmt, int fd, uint32_t size) {
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (map == MAP_FAILED) return;
    if (g.xkb_st)   xkb_state_unref(g.xkb_st);
    if (g.xkb_kmap) xkb_keymap_unref(g.xkb_kmap);
    g.xkb_kmap = xkb_keymap_new_from_buffer(g.xkb_ctx, map, size - 1,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (g.xkb_kmap) g.xkb_st = xkb_state_new(g.xkb_kmap);
}

void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
    input::dispatch_keyboard_focus(true);
}

void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {
    input::dispatch_keyboard_focus(false);
}

void kb_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    if (!g.xkb_st) return;
    uint32_t kc = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(g.xkb_st, kc);
    const bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);

    KeyEvent e{};
    e.code             = keysym_to_keycode(sym);
    e.windows_key_code = keysym_to_vkey(sym);
    e.action           = pressed ? KeyAction::Down : KeyAction::Up;
    e.modifiers        = g.modifiers;
    e.native_key_code  = static_cast<int>(key);
    e.is_system_key    = false;
    input::dispatch_key(e);

    if (pressed) {
        uint32_t cp = xkb_state_key_get_utf32(g.xkb_st, kc);
        if (cp > 0)
            input::dispatch_char(cp, g.modifiers, static_cast<int>(key), false);
    }
}

void kb_modifiers(void*, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
    if (g.xkb_st) {
        xkb_state_update_mask(g.xkb_st, dep, lat, lock, 0, 0, grp);
        g.modifiers = xkb_to_cef_mods();
    }
}

void kb_repeat(void*, wl_keyboard*, int32_t, int32_t) {}

const wl_keyboard_listener s_kb = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
    .key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat,
};

// --- Seat listener ----------------------------------------------------------

void seat_caps(void*, wl_seat* seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g.pointer) {
        g.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g.pointer, &s_ptr, nullptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g.keyboard) {
        g.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g.keyboard, &s_kb, nullptr);
    }
}

void seat_name(void*, wl_seat*, const char*) {}

const wl_seat_listener s_seat = {
    .capabilities = seat_caps,
    .name = seat_name,
};

// --- Cursor shape translation -----------------------------------------------

uint32_t cef_cursor_to_wl_shape(cef_cursor_type_t type) {
    switch (type) {
    case CT_CROSS:                      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
    case CT_HAND:                       return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    case CT_IBEAM:                      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    case CT_WAIT:                       return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
    case CT_HELP:                       return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP;
    case CT_EASTRESIZE:                 return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
    case CT_NORTHRESIZE:                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
    case CT_NORTHEASTRESIZE:            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
    case CT_NORTHWESTRESIZE:            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
    case CT_SOUTHRESIZE:                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
    case CT_SOUTHEASTRESIZE:            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
    case CT_SOUTHWESTRESIZE:            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
    case CT_WESTRESIZE:                 return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
    case CT_NORTHSOUTHRESIZE:           return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
    case CT_EASTWESTRESIZE:             return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
    case CT_NORTHEASTSOUTHWESTRESIZE:   return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
    case CT_NORTHWESTSOUTHEASTRESIZE:   return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
    case CT_COLUMNRESIZE:               return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE;
    case CT_ROWRESIZE:                  return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE;
    case CT_MOVE:                       return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
    case CT_VERTICALTEXT:               return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT;
    case CT_CELL:                       return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL;
    case CT_CONTEXTMENU:                return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU;
    case CT_ALIAS:                      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS;
    case CT_PROGRESS:                   return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS;
    case CT_NODROP:                     return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP;
    case CT_COPY:                       return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY;
    case CT_NOTALLOWED:                 return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
    case CT_ZOOMIN:                     return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN;
    case CT_ZOOMOUT:                    return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT;
    case CT_GRAB:                       return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
    case CT_GRABBING:                   return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
    case CT_MIDDLEPANNING:
    case CT_MIDDLE_PANNING_VERTICAL:
    case CT_MIDDLE_PANNING_HORIZONTAL:  return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL;
    default:                            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
}

// --- Input thread -----------------------------------------------------------

void input_thread_func() {
    int display_fd = wl_display_get_fd(g.display);
    struct pollfd fds[2] = {
        {display_fd, POLLIN, 0},
        {g_shutdown_event.fd(), POLLIN, 0},
    };
    while (true) {
        while (wl_display_prepare_read_queue(g.display, g.queue) != 0)
            wl_display_dispatch_queue_pending(g.display, g.queue);
        wl_display_flush(g.display);

        poll(fds, 2, -1);

        if (fds[0].revents & POLLIN) {
            wl_display_read_events(g.display);
        } else {
            wl_display_cancel_read(g.display);
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (fds[1].revents & POLLIN)
            break;

        wl_display_dispatch_queue_pending(g.display, g.queue);
    }
}

}  // namespace

// --- Public API -------------------------------------------------------------

void init(wl_display* display, wl_event_queue* queue) {
    g.display = display;
    g.queue   = queue;
    g.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
}

void attach_seat(wl_seat* seat) {
    g.seat = seat;
    wl_seat_add_listener(seat, &s_seat, nullptr);
}

void attach_cursor_shape_manager(wp_cursor_shape_manager_v1* mgr) {
    g.cursor_shape_manager = mgr;
}

void start_input_thread() {
    g.input_thread = std::thread(input_thread_func);
}

void cleanup() {
    if (g.input_thread.joinable()) g.input_thread.join();
    if (g.cursor_shape_device) {
        wp_cursor_shape_device_v1_destroy(g.cursor_shape_device);
        g.cursor_shape_device = nullptr;
    }
    if (g.cursor_shape_manager) {
        wp_cursor_shape_manager_v1_destroy(g.cursor_shape_manager);
        g.cursor_shape_manager = nullptr;
    }
    if (g.pointer)  { wl_pointer_destroy(g.pointer);   g.pointer = nullptr; }
    if (g.keyboard) { wl_keyboard_destroy(g.keyboard); g.keyboard = nullptr; }
    if (g.seat)     { wl_seat_destroy(g.seat);         g.seat = nullptr; }
    if (g.xkb_st)   { xkb_state_unref(g.xkb_st);       g.xkb_st = nullptr; }
    if (g.xkb_kmap) { xkb_keymap_unref(g.xkb_kmap);    g.xkb_kmap = nullptr; }
    if (g.xkb_ctx)  { xkb_context_unref(g.xkb_ctx);    g.xkb_ctx = nullptr; }
}

void set_cursor(cef_cursor_type_t type) {
    g.cursor_type = type;
    if (!g.pointer || !g.pointer_serial) return;
    if (type == CT_NONE) {
        wl_pointer_set_cursor(g.pointer, g.pointer_serial, nullptr, 0, 0);
    } else {
        if (!g.cursor_shape_device && g.cursor_shape_manager)
            g.cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
                g.cursor_shape_manager, g.pointer);
        if (g.cursor_shape_device)
            wp_cursor_shape_device_v1_set_shape(g.cursor_shape_device,
                g.pointer_serial, cef_cursor_to_wl_shape(type));
    }
    if (g.display) wl_display_flush(g.display);
}

}  // namespace input::wayland
