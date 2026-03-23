#pragma once

#include <SDL3/SDL_keycode.h>

// Maps SDL3 keycodes to platform key codes for CEF
// CEF key events need:
//   windows_key_code - cross-platform (Windows VK codes used on all platforms)
//   native_key_code  - platform-specific (Mac kVK_* on macOS, SDL key on Linux)
//
// Problem: SDL uses ASCII/Unicode for printable chars, but some ASCII values
// collide with Windows VK codes for different keys:
//   ',' (0x2C) = VK_SNAPSHOT (Print Screen)
//   '-' (0x2D) = VK_INSERT
//   '.' (0x2E) = VK_DELETE
//
// Windows Virtual Key codes (not available on non-Windows platforms)
namespace VK {
    constexpr int BACK        = 0x08;
    constexpr int TAB         = 0x09;
    constexpr int RETURN      = 0x0D;
    constexpr int ESCAPE      = 0x1B;
    constexpr int SPACE       = 0x20;
    constexpr int PRIOR       = 0x21;  // Page Up
    constexpr int NEXT        = 0x22;  // Page Down
    constexpr int END         = 0x23;
    constexpr int HOME        = 0x24;
    constexpr int LEFT        = 0x25;
    constexpr int UP          = 0x26;
    constexpr int RIGHT       = 0x27;
    constexpr int DOWN        = 0x28;
    constexpr int INSERT      = 0x2D;
    constexpr int DELETE_     = 0x2E;  // DELETE is a macro on some platforms
    constexpr int APPS        = 0x5D;  // Context menu key
    constexpr int NUMPAD0     = 0x60;
    constexpr int NUMPAD1     = 0x61;
    constexpr int NUMPAD2     = 0x62;
    constexpr int NUMPAD3     = 0x63;
    constexpr int NUMPAD4     = 0x64;
    constexpr int NUMPAD5     = 0x65;
    constexpr int NUMPAD6     = 0x66;
    constexpr int NUMPAD7     = 0x67;
    constexpr int NUMPAD8     = 0x68;
    constexpr int NUMPAD9     = 0x69;
    constexpr int MULTIPLY    = 0x6A;
    constexpr int ADD         = 0x6B;
    constexpr int SUBTRACT    = 0x6D;
    constexpr int DECIMAL     = 0x6E;
    constexpr int DIVIDE      = 0x6F;
    constexpr int F1          = 0x70;
    constexpr int F2          = 0x71;
    constexpr int F3          = 0x72;
    constexpr int F4          = 0x73;
    constexpr int F5          = 0x74;
    constexpr int F6          = 0x75;
    constexpr int F7          = 0x76;
    constexpr int F8          = 0x77;
    constexpr int F9          = 0x78;
    constexpr int F10         = 0x79;
    constexpr int F11         = 0x7A;
    constexpr int F12         = 0x7B;
    constexpr int BROWSER_BACK    = 0xA6;
    constexpr int BROWSER_FORWARD = 0xA7;
    constexpr int BROWSER_REFRESH = 0xA8;
    constexpr int BROWSER_STOP    = 0xA9;
    constexpr int BROWSER_HOME    = 0xAC;
    constexpr int VOLUME_MUTE     = 0xAD;
    constexpr int VOLUME_DOWN     = 0xAE;
    constexpr int VOLUME_UP       = 0xAF;
    constexpr int MEDIA_NEXT_TRACK  = 0xB0;
    constexpr int MEDIA_PREV_TRACK  = 0xB1;
    constexpr int MEDIA_STOP        = 0xB2;
    constexpr int MEDIA_PLAY_PAUSE  = 0xB3;
    constexpr int OEM_1       = 0xBA;  // ;:
    constexpr int OEM_PLUS    = 0xBB;  // =+
    constexpr int OEM_COMMA   = 0xBC;  // ,<
    constexpr int OEM_MINUS   = 0xBD;  // -_
    constexpr int OEM_PERIOD  = 0xBE;  // .>
    constexpr int OEM_2       = 0xBF;  // /?
    constexpr int OEM_3       = 0xC0;  // `~
    constexpr int OEM_4       = 0xDB;  // [{
    constexpr int OEM_5       = 0xDC;  // \|
    constexpr int OEM_6       = 0xDD;  // ]}
    constexpr int OEM_7       = 0xDE;  // '"
}

inline int sdlKeyToWindowsVK(int sdlKey) {
    switch (sdlKey) {
        // Navigation keys
        case SDLK_LEFT:     return VK::LEFT;
        case SDLK_RIGHT:    return VK::RIGHT;
        case SDLK_UP:       return VK::UP;
        case SDLK_DOWN:     return VK::DOWN;
        case SDLK_HOME:     return VK::HOME;
        case SDLK_END:      return VK::END;
        case SDLK_PAGEUP:   return VK::PRIOR;
        case SDLK_PAGEDOWN: return VK::NEXT;

        // Editing keys
        case SDLK_BACKSPACE: return VK::BACK;
        case SDLK_TAB:       return VK::TAB;
        case SDLK_RETURN:    return VK::RETURN;
        case SDLK_ESCAPE:    return VK::ESCAPE;
        case SDLK_SPACE:     return VK::SPACE;
        case SDLK_DELETE:    return VK::DELETE_;
        case SDLK_INSERT:    return VK::INSERT;

        // Function keys
        case SDLK_F1:  return VK::F1;
        case SDLK_F2:  return VK::F2;
        case SDLK_F3:  return VK::F3;
        case SDLK_F4:  return VK::F4;
        case SDLK_F5:  return VK::F5;
        case SDLK_F6:  return VK::F6;
        case SDLK_F7:  return VK::F7;
        case SDLK_F8:  return VK::F8;
        case SDLK_F9:  return VK::F9;
        case SDLK_F10: return VK::F10;
        case SDLK_F11: return VK::F11;
        case SDLK_F12: return VK::F12;

        // Numpad
        case SDLK_KP_0:        return VK::NUMPAD0;
        case SDLK_KP_1:        return VK::NUMPAD1;
        case SDLK_KP_2:        return VK::NUMPAD2;
        case SDLK_KP_3:        return VK::NUMPAD3;
        case SDLK_KP_4:        return VK::NUMPAD4;
        case SDLK_KP_5:        return VK::NUMPAD5;
        case SDLK_KP_6:        return VK::NUMPAD6;
        case SDLK_KP_7:        return VK::NUMPAD7;
        case SDLK_KP_8:        return VK::NUMPAD8;
        case SDLK_KP_9:        return VK::NUMPAD9;
        case SDLK_KP_DIVIDE:   return VK::DIVIDE;
        case SDLK_KP_MULTIPLY: return VK::MULTIPLY;
        case SDLK_KP_MINUS:    return VK::SUBTRACT;
        case SDLK_KP_PLUS:     return VK::ADD;
        case SDLK_KP_ENTER:    return VK::RETURN;
        case SDLK_KP_PERIOD:   return VK::DECIMAL;

        // Media keys
        case SDLK_MUTE:                 return VK::VOLUME_MUTE;
        case SDLK_VOLUMEUP:             return VK::VOLUME_UP;
        case SDLK_VOLUMEDOWN:           return VK::VOLUME_DOWN;
        case SDLK_MEDIA_PLAY:           return VK::MEDIA_PLAY_PAUSE;
        case SDLK_MEDIA_PAUSE:          return VK::MEDIA_PLAY_PAUSE;
        case SDLK_MEDIA_NEXT_TRACK:     return VK::MEDIA_NEXT_TRACK;
        case SDLK_MEDIA_PREVIOUS_TRACK: return VK::MEDIA_PREV_TRACK;
        case SDLK_MEDIA_STOP:           return VK::MEDIA_STOP;
        case SDLK_MEDIA_PLAY_PAUSE:     return VK::MEDIA_PLAY_PAUSE;

        // Browser/navigation keys
        case SDLK_AC_BACK:    return VK::BROWSER_BACK;
        case SDLK_AC_FORWARD: return VK::BROWSER_FORWARD;
        case SDLK_AC_REFRESH: return VK::BROWSER_REFRESH;
        case SDLK_AC_STOP:    return VK::BROWSER_STOP;
        case SDLK_AC_HOME:    return VK::BROWSER_HOME;

        // Context menu (right-click equivalent)
        case SDLK_APPLICATION: return VK::APPS;
        case SDLK_MENU:        return VK::APPS;

        // Punctuation - these collide with Windows VK codes if passed through
        case SDLK_COMMA:        return VK::OEM_COMMA;
        case SDLK_MINUS:        return VK::OEM_MINUS;
        case SDLK_PERIOD:       return VK::OEM_PERIOD;
        case SDLK_SLASH:        return VK::OEM_2;
        case SDLK_SEMICOLON:    return VK::OEM_1;
        case SDLK_EQUALS:       return VK::OEM_PLUS;
        case SDLK_LEFTBRACKET:  return VK::OEM_4;
        case SDLK_BACKSLASH:    return VK::OEM_5;
        case SDLK_RIGHTBRACKET: return VK::OEM_6;
        case SDLK_GRAVE:        return VK::OEM_3;
        case SDLK_APOSTROPHE:   return VK::OEM_7;

        // Letters - SDL uses lowercase, Windows VK uses uppercase
        case SDLK_A: return 'A';
        case SDLK_B: return 'B';
        case SDLK_C: return 'C';
        case SDLK_D: return 'D';
        case SDLK_E: return 'E';
        case SDLK_F: return 'F';
        case SDLK_G: return 'G';
        case SDLK_H: return 'H';
        case SDLK_I: return 'I';
        case SDLK_J: return 'J';
        case SDLK_K: return 'K';
        case SDLK_L: return 'L';
        case SDLK_M: return 'M';
        case SDLK_N: return 'N';
        case SDLK_O: return 'O';
        case SDLK_P: return 'P';
        case SDLK_Q: return 'Q';
        case SDLK_R: return 'R';
        case SDLK_S: return 'S';
        case SDLK_T: return 'T';
        case SDLK_U: return 'U';
        case SDLK_V: return 'V';
        case SDLK_W: return 'W';
        case SDLK_X: return 'X';
        case SDLK_Y: return 'Y';
        case SDLK_Z: return 'Z';

        // Numbers pass through (0x30-0x39 = '0'-'9' = VK_0-VK_9)
        default: return sdlKey;
    }
}

#ifdef __APPLE__
// Mac Carbon virtual key codes (kVK_* from Events.h)
namespace kVK {
    constexpr int ANSI_A        = 0x00;
    constexpr int ANSI_S        = 0x01;
    constexpr int ANSI_D        = 0x02;
    constexpr int ANSI_F        = 0x03;
    constexpr int ANSI_H        = 0x04;
    constexpr int ANSI_G        = 0x05;
    constexpr int ANSI_Z        = 0x06;
    constexpr int ANSI_X        = 0x07;
    constexpr int ANSI_C        = 0x08;
    constexpr int ANSI_V        = 0x09;
    constexpr int ANSI_B        = 0x0B;
    constexpr int ANSI_Q        = 0x0C;
    constexpr int ANSI_W        = 0x0D;
    constexpr int ANSI_E        = 0x0E;
    constexpr int ANSI_R        = 0x0F;
    constexpr int ANSI_Y        = 0x10;
    constexpr int ANSI_T        = 0x11;
    constexpr int ANSI_1        = 0x12;
    constexpr int ANSI_2        = 0x13;
    constexpr int ANSI_3        = 0x14;
    constexpr int ANSI_4        = 0x15;
    constexpr int ANSI_6        = 0x16;
    constexpr int ANSI_5        = 0x17;
    constexpr int ANSI_9        = 0x19;
    constexpr int ANSI_7        = 0x1A;
    constexpr int ANSI_8        = 0x1C;
    constexpr int ANSI_0        = 0x1D;
    constexpr int ANSI_O        = 0x1F;
    constexpr int ANSI_U        = 0x20;
    constexpr int ANSI_I        = 0x22;
    constexpr int ANSI_P        = 0x23;
    constexpr int ANSI_L        = 0x25;
    constexpr int ANSI_J        = 0x26;
    constexpr int ANSI_K        = 0x28;
    constexpr int ANSI_N        = 0x2D;
    constexpr int ANSI_M        = 0x2E;
    constexpr int Return        = 0x24;
    constexpr int Tab           = 0x30;
    constexpr int Space         = 0x31;
    constexpr int Delete        = 0x33;  // Backspace
    constexpr int Escape        = 0x35;
    constexpr int F5            = 0x60;
    constexpr int F11           = 0x67;
    constexpr int Home          = 0x73;
    constexpr int PageUp        = 0x74;
    constexpr int ForwardDelete = 0x75;
    constexpr int End           = 0x77;
    constexpr int PageDown      = 0x79;
    constexpr int LeftArrow     = 0x7B;
    constexpr int RightArrow    = 0x7C;
    constexpr int DownArrow     = 0x7D;
    constexpr int UpArrow       = 0x7E;
}

inline int sdlKeyToMacNative(int sdlKey) {
    switch (sdlKey) {
        // Navigation
        case SDLK_LEFT:     return kVK::LeftArrow;
        case SDLK_RIGHT:    return kVK::RightArrow;
        case SDLK_UP:       return kVK::UpArrow;
        case SDLK_DOWN:     return kVK::DownArrow;
        case SDLK_HOME:     return kVK::Home;
        case SDLK_END:      return kVK::End;
        case SDLK_PAGEUP:   return kVK::PageUp;
        case SDLK_PAGEDOWN: return kVK::PageDown;

        // Editing
        case SDLK_BACKSPACE: return kVK::Delete;
        case SDLK_TAB:       return kVK::Tab;
        case SDLK_RETURN:    return kVK::Return;
        case SDLK_ESCAPE:    return kVK::Escape;
        case SDLK_SPACE:     return kVK::Space;
        case SDLK_DELETE:    return kVK::ForwardDelete;

        // Function keys
        case SDLK_F5:  return kVK::F5;
        case SDLK_F11: return kVK::F11;

        // Letters - must map ALL to avoid collisions with kVK codes
        // (e.g., SDLK_S = 0x73 = kVK_Home without explicit mapping)
        case SDLK_A: return kVK::ANSI_A;
        case SDLK_B: return kVK::ANSI_B;
        case SDLK_C: return kVK::ANSI_C;
        case SDLK_D: return kVK::ANSI_D;
        case SDLK_E: return kVK::ANSI_E;
        case SDLK_F: return kVK::ANSI_F;
        case SDLK_G: return kVK::ANSI_G;
        case SDLK_H: return kVK::ANSI_H;
        case SDLK_I: return kVK::ANSI_I;
        case SDLK_J: return kVK::ANSI_J;
        case SDLK_K: return kVK::ANSI_K;
        case SDLK_L: return kVK::ANSI_L;
        case SDLK_M: return kVK::ANSI_M;
        case SDLK_N: return kVK::ANSI_N;
        case SDLK_O: return kVK::ANSI_O;
        case SDLK_P: return kVK::ANSI_P;
        case SDLK_Q: return kVK::ANSI_Q;
        case SDLK_R: return kVK::ANSI_R;
        case SDLK_S: return kVK::ANSI_S;
        case SDLK_T: return kVK::ANSI_T;
        case SDLK_U: return kVK::ANSI_U;
        case SDLK_V: return kVK::ANSI_V;
        case SDLK_W: return kVK::ANSI_W;
        case SDLK_X: return kVK::ANSI_X;
        case SDLK_Y: return kVK::ANSI_Y;
        case SDLK_Z: return kVK::ANSI_Z;

        // On MacOS, 0 and 3 SDL codes do not correspond to the correct kVK
        // codes and must be mapped manually
        case SDLK_0: return kVK::ANSI_0;
        case SDLK_3: return kVK::ANSI_3;

        default: return sdlKey;
    }
}
#endif
