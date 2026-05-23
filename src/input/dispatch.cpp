#include "dispatch.h"

#include "input.h"
#include "jfn_hotkey.h"
#include "../common.h"
#include "../cef/cef_client.h"

#include "include/internal/cef_types.h"

#if defined(__linux__)
#include "keysym_map.h"
#include <xkbcommon/xkbcommon.h>
#endif

#include <mutex>

// Rust Browsers active query (src/jfn_cef/src/browsers.rs).
extern "C" JfnCefLayer* jfn_browsers_active(void);

namespace input {
namespace {

std::mutex g_last_pos_mtx;
LastMousePos g_last_pos;  // guarded by g_last_pos_mtx

JfnCefLayer* active_layer() {
    return jfn_browsers_active();
}

int to_cef_button(MouseButton b) {
    switch (b) {
    case MouseButton::Left:   return MBT_LEFT;
    case MouseButton::Right:  return MBT_RIGHT;
    case MouseButton::Middle: return MBT_MIDDLE;
    }
    return MBT_LEFT;
}

}  // namespace

LastMousePos last_mouse_pos() {
    std::lock_guard<std::mutex> lk(g_last_pos_mtx);
    return g_last_pos;
}

extern "C" int jfn_input_last_mouse_pos(int32_t* out_x, int32_t* out_y,
                                        uint32_t* out_modifiers) {
    std::lock_guard<std::mutex> lk(g_last_pos_mtx);
    if (out_x) *out_x = g_last_pos.x;
    if (out_y) *out_y = g_last_pos.y;
    if (out_modifiers) *out_modifiers = g_last_pos.modifiers;
    return g_last_pos.valid ? 1 : 0;
}

void dispatch_key(const KeyEvent& e) {
    if (e.action == KeyAction::Down) {
        switch (jfn_hotkey_classify_keydown(e.windows_key_code, e.modifiers)) {
        case 1: initiate_shutdown(); return;
        case 2: g_platform.toggle_fullscreen(); return;
        default: break;
        }
    }
    auto* l = active_layer();
    if (!l) return;
    int type_ = (e.action == KeyAction::Down) ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
    jfn_cef_layer_send_key_event(
        l, type_, e.modifiers, e.windows_key_code, e.native_key_code,
        e.is_system_key, static_cast<uint16_t>(e.character),
        static_cast<uint16_t>(e.unmodified_character));
}

void dispatch_char(uint32_t codepoint, uint32_t modifiers,
                   int native_key_code, bool is_system_key) {
    if (codepoint == 0 || codepoint >= 0x10FFFF) return;
    auto* l = active_layer();
    if (!l) return;
    jfn_cef_layer_send_key_event(
        l, KEYEVENT_CHAR, modifiers, static_cast<int>(codepoint),
        native_key_code, is_system_key,
        static_cast<uint16_t>(codepoint),
        static_cast<uint16_t>(codepoint));
}

void dispatch_mouse_button(const MouseButtonEvent& e) {
    auto* l = active_layer();
    if (!l) return;
    jfn_cef_layer_send_mouse_click(
        l, e.x, e.y, e.modifiers, to_cef_button(e.button),
        !e.pressed, e.click_count);
}

void dispatch_mouse_move(const MouseMoveEvent& e) {
    {
        std::lock_guard<std::mutex> lk(g_last_pos_mtx);
        if (e.leave) {
            g_last_pos.valid = false;
        } else {
            g_last_pos.valid = true;
            g_last_pos.x = e.x;
            g_last_pos.y = e.y;
            g_last_pos.modifiers = e.modifiers;
        }
    }
    auto* l = active_layer();
    if (!l) return;
    jfn_cef_layer_send_mouse_move(l, e.x, e.y, e.modifiers, e.leave);
}

void dispatch_history_nav(bool forward) {
    auto* l = active_layer();
    if (!l) return;
    if (forward) {
        if (jfn_cef_layer_can_go_forward(l)) jfn_cef_layer_go_forward(l);
    } else {
        if (jfn_cef_layer_can_go_back(l)) jfn_cef_layer_go_back(l);
    }
}

void dispatch_scroll(const ScrollEvent& e) {
    auto* l = active_layer();
    if (!l) return;
    uint32_t mods = e.modifiers;
    if (e.precise) mods |= EVENTFLAG_PRECISION_SCROLLING_DELTA;
    jfn_cef_layer_send_mouse_wheel(l, e.x, e.y, mods, e.dx, e.dy);
}

void dispatch_keyboard_focus(bool gained) {
    auto* l = active_layer();
    if (l) jfn_cef_layer_set_focus(l, gained);
}

}  // namespace input

// extern "C" dispatch shims callable from Rust. Reconstruct the C++ event
// structs from primitive args and forward to the dispatch_* functions above.
extern "C" void jfn_input_dispatch_mouse_move(int32_t x, int32_t y, uint32_t mods, int leave) {
    input::dispatch_mouse_move({
        .x = x, .y = y, .modifiers = mods, .leave = leave != 0,
    });
}

extern "C" void jfn_input_dispatch_mouse_button(uint32_t button_code, int pressed,
                                                int32_t x, int32_t y, uint32_t mods) {
    input::MouseButton btn;
    switch (button_code) {
    case 0x110: btn = input::MouseButton::Left;   break;
    case 0x111: btn = input::MouseButton::Right;  break;
    case 0x112: btn = input::MouseButton::Middle; break;
    default: return;
    }
    input::dispatch_mouse_button({
        .button = btn,
        .pressed = pressed != 0,
        .x = x, .y = y,
        .click_count = 1,
        .modifiers = mods,
    });
}

extern "C" void jfn_input_dispatch_scroll(int32_t x, int32_t y, int32_t dx, int32_t dy,
                                          uint32_t mods) {
    input::dispatch_scroll({
        .x = x, .y = y, .dx = dx, .dy = dy, .modifiers = mods,
    });
}

extern "C" void jfn_input_dispatch_history_nav(int forward) {
    input::dispatch_history_nav(forward != 0);
}

extern "C" void jfn_input_dispatch_keyboard_focus(int gained) {
    input::dispatch_keyboard_focus(gained != 0);
}

#if defined(__linux__)
extern "C" void jfn_input_dispatch_key_raw(uint32_t keysym, uint32_t native_code,
                                           uint32_t mods, int pressed) {
    if (keysym == XKB_KEY_XF86Back || keysym == XKB_KEY_XF86Forward) {
        if (pressed) input::dispatch_history_nav(keysym == XKB_KEY_XF86Forward);
        return;
    }
    input::KeyEvent e{};
    e.code             = input::keysym_to_keycode(keysym);
    e.windows_key_code = input::keysym_to_vkey(keysym);
    e.action           = pressed ? input::KeyAction::Down : input::KeyAction::Up;
    e.modifiers        = mods;
    // CEF on Linux expects an X11 keycode (evdev keycode + 8) for native_key_code.
    e.native_key_code  = static_cast<int>(native_code) + 8;
    e.is_system_key    = false;
    input::dispatch_key(e);
}
#endif

extern "C" void jfn_input_dispatch_char(uint32_t codepoint, uint32_t mods,
                                        uint32_t native_code) {
    input::dispatch_char(codepoint, mods, static_cast<int>(native_code), false);
}
