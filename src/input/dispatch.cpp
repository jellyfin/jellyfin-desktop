#include "dispatch.h"

#include "input.h"
#include "hotkeys.h"
#include "logging.h"

#include "include/cef_browser.h"

#include <mutex>

namespace input {
namespace {

std::mutex g_active_mtx;
CefRefPtr<CefBrowser> g_active;  // guarded by g_active_mtx

std::mutex g_last_pos_mtx;
struct LastPos {
    bool     valid = false;
    int      x = 0, y = 0;
    uint32_t modifiers = 0;
} g_last_pos;  // guarded by g_last_pos_mtx

cef_mouse_button_type_t to_cef_button(MouseButton b) {
    switch (b) {
    case MouseButton::Left:   return MBT_LEFT;
    case MouseButton::Right:  return MBT_RIGHT;
    case MouseButton::Middle: return MBT_MIDDLE;
    }
    return MBT_LEFT;
}

}  // namespace

CefRefPtr<CefBrowser> active_browser() {
    std::lock_guard<std::mutex> lk(g_active_mtx);
    return g_active;
}

void set_active_browser(CefRefPtr<CefBrowser> browser) {
    CefRefPtr<CefBrowser> prev;
    {
        std::lock_guard<std::mutex> lk(g_active_mtx);
        if (g_active.get() == browser.get()) return;
        prev = g_active;
        g_active = browser;
    }
    LOG_INFO(LOG_PLATFORM, "[INPUT] set_active_browser prev={} new={}",
             static_cast<void*>(prev.get()), static_cast<void*>(browser.get()));
    if (prev)    prev->GetHost()->SetFocus(false);
    if (browser) browser->GetHost()->SetFocus(true);

    // Leave-then-move forces the renderer to re-emit OnCursorChange even
    // when its cached cursor matches the new hit-test result; otherwise the
    // platform cursor stays on whatever the previous active browser set.
    if (browser) {
        LastPos pos;
        {
            std::lock_guard<std::mutex> lk(g_last_pos_mtx);
            pos = g_last_pos;
        }
        if (pos.valid) {
            CefMouseEvent me{};
            me.x = pos.x; me.y = pos.y; me.modifiers = pos.modifiers;
            browser->GetHost()->SendMouseMoveEvent(me, true);
            browser->GetHost()->SendMouseMoveEvent(me, false);
        }
    }
}

void dispatch_key(const KeyEvent& e) {
    if (e.action == KeyAction::Down && hotkey_try_consume(e)) return;

    auto b = active_browser();
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

    auto b = active_browser();
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
    auto b = active_browser();
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
    auto b = active_browser();
    if (!b) return;
    CefMouseEvent me{};
    me.x = e.x; me.y = e.y; me.modifiers = e.modifiers;
    b->GetHost()->SendMouseMoveEvent(me, e.leave);
}

void dispatch_scroll(const ScrollEvent& e) {
    auto b = active_browser();
    if (!b) return;
    CefMouseEvent me{};
    me.x = e.x; me.y = e.y; me.modifiers = e.modifiers;
    b->GetHost()->SendMouseWheelEvent(me, e.dx, e.dy);
}

void dispatch_keyboard_focus(bool gained) {
    auto b = active_browser();
    if (b) b->GetHost()->SetFocus(gained);
}

}  // namespace input
