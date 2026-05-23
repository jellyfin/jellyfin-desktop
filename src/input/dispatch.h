#pragma once

#include "input.h"

#include <cstdint>

// Inline C++ shim that forwards into the Rust input crate
// (src/input/src/lib.rs). Platform translators in input_windows.cpp and
// input_macos.mm call the namespace-scoped wrappers below; the real
// dispatch logic (active-browser lookup, hotkey classification, CEF
// forwarding) lives entirely in Rust.

extern "C" {
void jfn_input_dispatch_mouse_move(int32_t x, int32_t y, uint32_t mods, int leave);
void jfn_input_dispatch_mouse_button(uint32_t button_code, int pressed,
                                     int32_t x, int32_t y, uint32_t mods);
void jfn_input_dispatch_scroll(int32_t x, int32_t y, int32_t dx, int32_t dy, uint32_t mods);
void jfn_input_dispatch_scroll_precise(int32_t x, int32_t y, int32_t dx, int32_t dy,
                                       uint32_t mods, int precise);
void jfn_input_dispatch_history_nav(int forward);
void jfn_input_dispatch_keyboard_focus(int gained);
void jfn_input_dispatch_char(uint32_t codepoint, uint32_t mods, uint32_t native_code);
void jfn_input_dispatch_char_sys(uint32_t codepoint, uint32_t mods,
                                 uint32_t native_code, int is_system_key);
void jfn_input_dispatch_key_full(int pressed, int32_t windows_key_code,
                                 int32_t native_key_code, uint32_t modifiers,
                                 uint16_t character, uint16_t unmodified_character,
                                 int is_system_key);
}

namespace input {

inline void dispatch_key(const KeyEvent& e) {
    jfn_input_dispatch_key_full(
        e.action == KeyAction::Down ? 1 : 0,
        e.windows_key_code,
        e.native_key_code,
        e.modifiers,
        e.character,
        e.unmodified_character,
        e.is_system_key ? 1 : 0);
}

inline void dispatch_char(uint32_t codepoint, uint32_t modifiers,
                          int native_key_code, bool is_system_key) {
    jfn_input_dispatch_char_sys(codepoint, modifiers,
                                static_cast<uint32_t>(native_key_code),
                                is_system_key ? 1 : 0);
}

inline void dispatch_mouse_button(const MouseButtonEvent& e) {
    uint32_t code;
    switch (e.button) {
    case MouseButton::Left:   code = 0x110; break;
    case MouseButton::Right:  code = 0x111; break;
    case MouseButton::Middle: code = 0x112; break;
    default: return;
    }
    jfn_input_dispatch_mouse_button(code, e.pressed ? 1 : 0, e.x, e.y, e.modifiers);
}

inline void dispatch_mouse_move(const MouseMoveEvent& e) {
    jfn_input_dispatch_mouse_move(e.x, e.y, e.modifiers, e.leave ? 1 : 0);
}

inline void dispatch_scroll(const ScrollEvent& e) {
    jfn_input_dispatch_scroll_precise(e.x, e.y, e.dx, e.dy, e.modifiers,
                                      e.precise ? 1 : 0);
}

inline void dispatch_history_nav(bool forward) {
    jfn_input_dispatch_history_nav(forward ? 1 : 0);
}

inline void dispatch_keyboard_focus(bool gained) {
    jfn_input_dispatch_keyboard_focus(gained ? 1 : 0);
}

}  // namespace input
