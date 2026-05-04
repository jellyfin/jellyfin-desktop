#pragma once

#include <cstdint>

// Common input vocabulary.
//
// Platform translators (input_wayland.cpp, input_windows.cpp, input_macos.mm)
// translate native input events into these structs and hand them off to the
// dispatch layer. No file outside src/input/ should reference these types.

namespace input {

// Logical key identifier. Platform translators map native key codes to this
// enum. Dispatch uses the code both for hotkey matching and for building the
// windows_key_code field of CefKeyEvent when forwarding to the browser.
enum class KeyCode : int {
    Unknown = 0,
    // Letters
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    // Digits
    Digit0, Digit1, Digit2, Digit3, Digit4,
    Digit5, Digit6, Digit7, Digit8, Digit9,
    // Function keys
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    // Navigation
    ArrowLeft, ArrowRight, ArrowUp, ArrowDown,
    Home, End, PageUp, PageDown,
    // Editing
    Tab, Return, Escape, Backspace, Delete, Space, Insert,
    // Modifiers (reported so translators don't have to drop them, but
    // rarely bound as hotkeys)
    Shift, Control, Alt, Meta, CapsLock,
};

enum class KeyAction { Down, Up };

// Key event.
//
// `modifiers` is a bitmask matching CEF's EVENTFLAG_* constants (from
// include/internal/cef_types.h). Translators build it directly so dispatch
// can pass it through to CefKeyEvent without remapping.
//
// `code` is used for hotkey matching. `windows_key_code` is the Windows VK
// code forwarded directly to CefKeyEvent — translators populate it from
// their native source (WPARAM on Windows, a VK lookup on Wayland/macOS)
// so dispatch never has to re-derive it.
//
// Character input is normally a separate concern delivered via
// dispatch_char(), but `character` / `unmodified_character` MUST also be
// populated on every keydown/keyup on macOS. CEF's macOS TranslateWebKey-
// Event builds a synthetic NSEvent from these fields; if both are zero it
// falls through to NSEventTypeFlagsChanged (a modifier-key event), which
// Blink then processes through a different editor path and causes keys
// like Backspace and Tab to fire their default action twice. Translators
// on other platforms may leave these at 0 — CEF's non-mac paths derive
// character data from native_key_code / windows_key_code instead.
struct KeyEvent {
    KeyCode   code;
    int       windows_key_code;
    KeyAction action;
    uint32_t  modifiers;
    int       native_key_code;       // forwarded to CefKeyEvent.native_key_code
    bool      is_system_key;         // Windows WM_SYSKEY*; false on Wayland/macOS
    uint16_t  character;             // CefKeyEvent.character (required on macOS)
    uint16_t  unmodified_character;  // CefKeyEvent.unmodified_character (required on macOS)
};

enum class MouseButton { Left, Right, Middle };

struct MouseButtonEvent {
    MouseButton button;
    bool        pressed;
    int         x, y;
    int         click_count;
    uint32_t    modifiers;
};

struct MouseMoveEvent {
    int      x, y;
    uint32_t modifiers;
    bool     leave;  // pointer left the window
};

struct ScrollEvent {
    int      x, y;
    int      dx, dy;  // wheel deltas (pixels)
    uint32_t modifiers;
    bool     precise = false;  // macOS trackpad precision scrolling
};

}  // namespace input
