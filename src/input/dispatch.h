#pragma once

#include <cstdint>

#include "include/cef_base.h"

class CefBrowser;

namespace input {

struct KeyEvent;
struct MouseButtonEvent;
struct MouseMoveEvent;
struct ScrollEvent;

// Platform translators call these with common input events after
// translating the native event. Dispatch resolves the active browser via
// g_browsers->active() and forwards.
void dispatch_key(const KeyEvent&);

// Character input (WM_CHAR, Wayland's utf32 sidecar, NSEvent characters).
void dispatch_char(uint32_t codepoint, uint32_t modifiers,
                   int native_key_code, bool is_system_key);

void dispatch_mouse_button(const MouseButtonEvent&);
void dispatch_mouse_move(const MouseMoveEvent&);
void dispatch_scroll(const ScrollEvent&);

// Mouse "back"/"forward" side buttons.
void dispatch_history_nav(bool forward);

// Called by platform translators when the native window gains or loses
// keyboard focus.
void dispatch_keyboard_focus(bool gained);

// Last observed mouse position for the currently-active browser.
// Consumed by Browsers::setActive to issue a leave-then-move so the
// cursor shape re-emits on the new target. Invalid until the first
// dispatch_mouse_move call, or after a leave.
struct LastMousePos {
    bool     valid = false;
    int      x = 0, y = 0;
    uint32_t modifiers = 0;
};
LastMousePos last_mouse_pos();

}  // namespace input
