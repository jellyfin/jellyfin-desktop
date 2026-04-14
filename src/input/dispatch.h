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
// translating the native event (wl_pointer / WndProc / NSEvent →
// KeyEvent / MouseButtonEvent / ScrollEvent). Dispatch runs hotkey
// checks (for keys) and forwards the event to the current active
// browser.
void dispatch_key(const KeyEvent&);

// Character input (WM_CHAR, Wayland's utf32 sidecar, NSEvent characters).
// Delivered separately from dispatch_key so translators don't encode a
// key-plus-character union.
void dispatch_char(uint32_t codepoint, uint32_t modifiers,
                   int native_key_code, bool is_system_key);

void dispatch_mouse_button(const MouseButtonEvent&);
void dispatch_mouse_move(const MouseMoveEvent&);
void dispatch_scroll(const ScrollEvent&);

// Called by platform translators when the native window gains or loses
// keyboard focus (wl_keyboard enter/leave, WM_SETFOCUS/WM_KILLFOCUS,
// NSWindow becomeKey/resignKey). Propagates to the currently active
// browser's CefBrowserHost::SetFocus.
void dispatch_keyboard_focus(bool gained);

// Set which CEF browser receives all subsequent input events. Called by
// CEF client lifecycle code (src/cef/cef_client.cpp) whenever the target
// changes. The input layer is deliberately ignorant of *why* — it just
// knows events now go here.
//
// When the active browser changes, dispatch propagates CEF focus:
// SetFocus(false) on the previous browser, SetFocus(true) on the new one,
// so text inputs get a blinking caret.
//
// Pass nullptr to disable forwarding (e.g. during shutdown).
void set_active_browser(CefRefPtr<CefBrowser> browser);

}  // namespace input
