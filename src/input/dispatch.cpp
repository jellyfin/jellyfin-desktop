#include "dispatch.h"

#include "input.h"
#include "hotkeys.h"
#include "logging.h"
#include "../browser/browsers.h"
#include "../cef/cef_client.h"

#include "include/internal/cef_types.h"

#include <mutex>

namespace input {
namespace {

std::mutex g_last_pos_mtx;
LastMousePos g_last_pos;  // guarded by g_last_pos_mtx

CefRefPtr<CefLayer> active_layer() {
    return g_browsers ? g_browsers->active() : nullptr;
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

void dispatch_key(const KeyEvent& e) {
    if (e.action == KeyAction::Down && hotkey_try_consume(e)) return;
    auto l = active_layer();
    if (!l) return;
    int type_ = (e.action == KeyAction::Down) ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
    l->sendKeyEvent(type_, e.modifiers, e.windows_key_code, e.native_key_code,
                    e.is_system_key, static_cast<uint16_t>(e.character),
                    static_cast<uint16_t>(e.unmodified_character));
}

void dispatch_char(uint32_t codepoint, uint32_t modifiers,
                   int native_key_code, bool is_system_key) {
    if (codepoint == 0 || codepoint >= 0x10FFFF) return;
    auto l = active_layer();
    if (!l) return;
    l->sendKeyEvent(KEYEVENT_CHAR, modifiers, static_cast<int>(codepoint),
                    native_key_code, is_system_key,
                    static_cast<uint16_t>(codepoint),
                    static_cast<uint16_t>(codepoint));
}

void dispatch_mouse_button(const MouseButtonEvent& e) {
    auto l = active_layer();
    if (!l) return;
    l->sendMouseClick(e.x, e.y, e.modifiers, to_cef_button(e.button), !e.pressed, e.click_count);
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
    auto l = active_layer();
    if (!l) return;
    l->sendMouseMove(e.x, e.y, e.modifiers, e.leave);
}

void dispatch_history_nav(bool forward) {
    auto l = active_layer();
    if (!l) return;
    if (forward) {
        if (l->canGoForward()) l->goForward();
    } else {
        if (l->canGoBack()) l->goBack();
    }
}

void dispatch_scroll(const ScrollEvent& e) {
    auto l = active_layer();
    if (!l) return;
    uint32_t mods = e.modifiers;
    if (e.precise) mods |= EVENTFLAG_PRECISION_SCROLLING_DELTA;
    l->sendMouseWheel(e.x, e.y, mods, e.dx, e.dy);
}

void dispatch_keyboard_focus(bool gained) {
    auto l = active_layer();
    if (l) l->setFocus(gained);
}

}  // namespace input
