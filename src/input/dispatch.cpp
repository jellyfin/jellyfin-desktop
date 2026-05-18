#include "dispatch.h"

#include "input.h"
#include "hotkeys.h"
#include "logging.h"
#include "../browser/browsers.h"

#include "include/cef_browser.h"

#include <mutex>

namespace input {
namespace {

std::mutex g_last_pos_mtx;
LastMousePos g_last_pos;  // guarded by g_last_pos_mtx

CefRefPtr<CefBrowser> active() {
    return g_browsers ? g_browsers->active() : nullptr;
}

cef_mouse_button_type_t to_cef_button(MouseButton b) {
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

    auto b = active();
    if (!b) return;

    CefKeyEvent ce{};
    ce.windows_key_code     = e.windows_key_code;
    ce.native_key_code      = e.native_key_code;
    ce.modifiers            = e.modifiers;
    ce.is_system_key        = e.is_system_key;
    ce.character            = e.character;
    ce.unmodified_character = e.unmodified_character;
    ce.type = (e.action == KeyAction::Down) ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
    b->GetHost()->SendKeyEvent(ce);
}

void dispatch_char(uint32_t codepoint, uint32_t modifiers,
                   int native_key_code, bool is_system_key) {
    if (codepoint == 0 || codepoint >= 0x10FFFF) return;

    auto b = active();
    if (!b) return;

    CefKeyEvent ce{};
    ce.type             = KEYEVENT_CHAR;
    ce.character        = codepoint;
    ce.windows_key_code = static_cast<int>(codepoint);
    ce.modifiers        = modifiers;
    ce.native_key_code  = native_key_code;
    ce.is_system_key    = is_system_key;
    b->GetHost()->SendKeyEvent(ce);
}

void dispatch_mouse_button(const MouseButtonEvent& e) {
    auto b = active();
    if (!b) return;
    CefMouseEvent me{};
    me.x = e.x; me.y = e.y; me.modifiers = e.modifiers;
    b->GetHost()->SendMouseClickEvent(me, to_cef_button(e.button), !e.pressed, e.click_count);
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
    auto b = active();
    if (!b) return;
    CefMouseEvent me{};
    me.x = e.x; me.y = e.y; me.modifiers = e.modifiers;
    b->GetHost()->SendMouseMoveEvent(me, e.leave);
}

void dispatch_history_nav(bool forward) {
    auto b = active();
    if (!b) return;
    if (forward) {
        if (b->CanGoForward()) b->GoForward();
    } else {
        if (b->CanGoBack()) b->GoBack();
    }
}

void dispatch_scroll(const ScrollEvent& e) {
    auto b = active();
    if (!b) return;
    CefMouseEvent me{};
    me.x = e.x; me.y = e.y;
    me.modifiers = e.modifiers;
    if (e.precise) me.modifiers |= EVENTFLAG_PRECISION_SCROLLING_DELTA;
    b->GetHost()->SendMouseWheelEvent(me, e.dx, e.dy);
}

void dispatch_keyboard_focus(bool gained) {
    auto b = active();
    if (b) b->GetHost()->SetFocus(gained);
}

}  // namespace input
