#include "input_wayland.h"

#include "input.h"
#include "keysym_map.h"
#include "dispatch.h"
#include "jfn_input_wayland.h"

#include <xkbcommon/xkbcommon.h>

namespace input::wayland {
namespace {

JfnInputWayland* g_ctx = nullptr;

extern "C" void cb_mouse_move(int32_t x, int32_t y, uint32_t modifiers, int leave) {
    input::dispatch_mouse_move({
        .x = x, .y = y, .modifiers = modifiers, .leave = leave != 0,
    });
}

extern "C" void cb_mouse_button(uint32_t button_code, int pressed,
                                int32_t x, int32_t y, uint32_t modifiers) {
    MouseButton btn;
    switch (button_code) {
    case 0x110: btn = MouseButton::Left;   break;
    case 0x111: btn = MouseButton::Right;  break;
    case 0x112: btn = MouseButton::Middle; break;
    default: return;
    }
    input::dispatch_mouse_button({
        .button = btn,
        .pressed = pressed != 0,
        .x = x, .y = y,
        .click_count = 1,
        .modifiers = modifiers,
    });
}

extern "C" void cb_scroll(int32_t x, int32_t y, int32_t dx, int32_t dy, uint32_t modifiers) {
    input::dispatch_scroll({
        .x = x, .y = y, .dx = dx, .dy = dy, .modifiers = modifiers,
    });
}

extern "C" void cb_history_nav(int forward) {
    input::dispatch_history_nav(forward != 0);
}

extern "C" void cb_kb_focus(int gained) {
    input::dispatch_keyboard_focus(gained != 0);
}

extern "C" void cb_key(uint32_t keysym, uint32_t native_code,
                       uint32_t modifiers, int pressed) {
    // Browser/IR-remote history navigation keys (XF86Back on MCE remotes).
    if (keysym == XKB_KEY_XF86Back || keysym == XKB_KEY_XF86Forward) {
        if (pressed) input::dispatch_history_nav(keysym == XKB_KEY_XF86Forward);
        return;
    }
    KeyEvent e{};
    e.code             = input::keysym_to_keycode(keysym);
    e.windows_key_code = input::keysym_to_vkey(keysym);
    e.action           = pressed ? KeyAction::Down : KeyAction::Up;
    e.modifiers        = modifiers;
    e.native_key_code  = static_cast<int>(native_code);
    e.is_system_key    = false;
    input::dispatch_key(e);
}

extern "C" void cb_char(uint32_t codepoint, uint32_t modifiers, uint32_t native_code) {
    input::dispatch_char(codepoint, modifiers, static_cast<int>(native_code), false);
}

const JfnInputCallbacks s_callbacks = {
    .mouse_move   = cb_mouse_move,
    .mouse_button = cb_mouse_button,
    .scroll       = cb_scroll,
    .history_nav  = cb_history_nav,
    .kb_focus     = cb_kb_focus,
    .key          = cb_key,
    .char_        = cb_char,
};

}  // namespace

void init(wl_display* display) {
    g_ctx = jfn_input_wayland_init(display, &s_callbacks);
}

void start_input_thread() {
    if (g_ctx) jfn_input_wayland_start(g_ctx);
}

void cleanup() {
    if (g_ctx) {
        jfn_input_wayland_cleanup(g_ctx);
        g_ctx = nullptr;
    }
}

void set_cursor(cef_cursor_type_t type) {
    if (g_ctx) jfn_input_wayland_set_cursor(g_ctx, static_cast<uint32_t>(type));
}

}  // namespace input::wayland
