#pragma once

#include "input.h"
#include <xkbcommon/xkbcommon.h>

namespace input {

inline KeyCode keysym_to_keycode(xkb_keysym_t sym) {
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z)
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (sym - XKB_KEY_a));
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z)
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (sym - XKB_KEY_A));
    if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9)
        return static_cast<KeyCode>(static_cast<int>(KeyCode::Digit0) + (sym - XKB_KEY_0));
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12)
        return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + (sym - XKB_KEY_F1));

    switch (sym) {
    case XKB_KEY_Return:    return KeyCode::Return;
    case XKB_KEY_Escape:    return KeyCode::Escape;
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return KeyCode::Tab;
    case XKB_KEY_BackSpace: return KeyCode::Backspace;
    case XKB_KEY_space:     return KeyCode::Space;
    case XKB_KEY_Left:      return KeyCode::ArrowLeft;
    case XKB_KEY_Up:        return KeyCode::ArrowUp;
    case XKB_KEY_Right:     return KeyCode::ArrowRight;
    case XKB_KEY_Down:      return KeyCode::ArrowDown;
    case XKB_KEY_Home:      return KeyCode::Home;
    case XKB_KEY_End:       return KeyCode::End;
    case XKB_KEY_Page_Up:   return KeyCode::PageUp;
    case XKB_KEY_Page_Down: return KeyCode::PageDown;
    case XKB_KEY_Delete:    return KeyCode::Delete;
    case XKB_KEY_Insert:    return KeyCode::Insert;
    case XKB_KEY_Shift_L: case XKB_KEY_Shift_R:       return KeyCode::Shift;
    case XKB_KEY_Control_L: case XKB_KEY_Control_R:   return KeyCode::Control;
    case XKB_KEY_Alt_L: case XKB_KEY_Alt_R:           return KeyCode::Alt;
    case XKB_KEY_Super_L: case XKB_KEY_Super_R:       return KeyCode::Meta;
    case XKB_KEY_Caps_Lock:                           return KeyCode::CapsLock;
    default: return KeyCode::Unknown;
    }
}

// Windows VK code used to populate CefKeyEvent.windows_key_code. Broader
// than keysym_to_keycode because CEF needs a VK for every key, including
// punctuation and numpad keys not covered by the hotkey-facing KeyCode enum.
inline int keysym_to_vkey(xkb_keysym_t sym) {
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) return 'A' + (sym - XKB_KEY_a);
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) return sym;
    if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) return sym;
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) return 0x70 + (sym - XKB_KEY_F1);

    switch (sym) {
    case XKB_KEY_Return:    return 0x0D;
    case XKB_KEY_Escape:    return 0x1B;
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return 0x09;
    case XKB_KEY_BackSpace: return 0x08;
    case XKB_KEY_space:     return 0x20;
    case XKB_KEY_Left:      return 0x25;
    case XKB_KEY_Up:        return 0x26;
    case XKB_KEY_Right:     return 0x27;
    case XKB_KEY_Down:      return 0x28;
    case XKB_KEY_Home:      return 0x24;
    case XKB_KEY_End:       return 0x23;
    case XKB_KEY_Page_Up:   return 0x21;
    case XKB_KEY_Page_Down: return 0x22;
    case XKB_KEY_Delete:    return 0x2E;
    case XKB_KEY_Insert:    return 0x2D;
    // OEM punctuation — needed so Chromium can derive event.key (e.g. '>'
    // from Shift+Period) for DOM keydown handlers. Without a VK here,
    // jellyfin-web's keyboard shortcuts like '<'/'>' never match.
    case XKB_KEY_semicolon: case XKB_KEY_colon:        return 0xBA; // VK_OEM_1
    case XKB_KEY_equal: case XKB_KEY_plus:             return 0xBB; // VK_OEM_PLUS
    case XKB_KEY_comma: case XKB_KEY_less:             return 0xBC; // VK_OEM_COMMA
    case XKB_KEY_minus: case XKB_KEY_underscore:       return 0xBD; // VK_OEM_MINUS
    case XKB_KEY_period: case XKB_KEY_greater:         return 0xBE; // VK_OEM_PERIOD
    case XKB_KEY_slash: case XKB_KEY_question:         return 0xBF; // VK_OEM_2
    case XKB_KEY_grave: case XKB_KEY_asciitilde:       return 0xC0; // VK_OEM_3
    case XKB_KEY_bracketleft: case XKB_KEY_braceleft:  return 0xDB; // VK_OEM_4
    case XKB_KEY_backslash: case XKB_KEY_bar:          return 0xDC; // VK_OEM_5
    case XKB_KEY_bracketright: case XKB_KEY_braceright:return 0xDD; // VK_OEM_6
    case XKB_KEY_apostrophe: case XKB_KEY_quotedbl:    return 0xDE; // VK_OEM_7
    default: return 0;
    }
}

}  // namespace input
