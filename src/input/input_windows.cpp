#include "input_windows.h"

#include "input.h"
#include "dispatch.h"
#include "logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <cstdint>

namespace input::windows {
namespace {

struct State {
    HWND  mpv_hwnd  = nullptr;
    HWND  input_hwnd = nullptr;
    DWORD thread_id  = 0;

    cef_cursor_type_t cursor_type = CT_POINTER;
};

State g;

// --- Modifier helpers -------------------------------------------------------

bool IsKeyDown(WPARAM wp) {
    return (GetKeyState(static_cast<int>(wp)) & 0x8000) != 0;
}

uint32_t mouse_modifiers(WPARAM wp) {
    uint32_t m = 0;
    if (wp & MK_CONTROL) m |= EVENTFLAG_CONTROL_DOWN;
    if (wp & MK_SHIFT)   m |= EVENTFLAG_SHIFT_DOWN;
    if (IsKeyDown(VK_MENU)) m |= EVENTFLAG_ALT_DOWN;
    if (wp & MK_LBUTTON) m |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (wp & MK_RBUTTON) m |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    if (wp & MK_MBUTTON) m |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    return m;
}

// From cefclient/tests/shared/browser/util_win.cc
uint32_t keyboard_modifiers(WPARAM wp, LPARAM lp) {
    uint32_t m = 0;
    if (IsKeyDown(VK_SHIFT))   m |= EVENTFLAG_SHIFT_DOWN;
    if (IsKeyDown(VK_CONTROL)) m |= EVENTFLAG_CONTROL_DOWN;
    if (IsKeyDown(VK_MENU))    m |= EVENTFLAG_ALT_DOWN;
    if (::GetKeyState(VK_NUMLOCK) & 1) m |= EVENTFLAG_NUM_LOCK_ON;
    if (::GetKeyState(VK_CAPITAL) & 1) m |= EVENTFLAG_CAPS_LOCK_ON;

    switch (wp) {
    case VK_RETURN:
        if ((lp >> 16) & KF_EXTENDED) m |= EVENTFLAG_IS_KEY_PAD;
        break;
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_UP: case VK_DOWN:
    case VK_LEFT: case VK_RIGHT:
        if (!((lp >> 16) & KF_EXTENDED)) m |= EVENTFLAG_IS_KEY_PAD;
        break;
    case VK_NUMLOCK: case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2:
    case VK_NUMPAD3: case VK_NUMPAD4: case VK_NUMPAD5: case VK_NUMPAD6:
    case VK_NUMPAD7: case VK_NUMPAD8: case VK_NUMPAD9:
    case VK_DIVIDE: case VK_MULTIPLY: case VK_SUBTRACT: case VK_ADD:
    case VK_DECIMAL: case VK_CLEAR:
        m |= EVENTFLAG_IS_KEY_PAD;
        break;
    case VK_SHIFT:
        if (IsKeyDown(VK_LSHIFT)) m |= EVENTFLAG_IS_LEFT;
        else if (IsKeyDown(VK_RSHIFT)) m |= EVENTFLAG_IS_RIGHT;
        break;
    case VK_CONTROL:
        if (IsKeyDown(VK_LCONTROL)) m |= EVENTFLAG_IS_LEFT;
        else if (IsKeyDown(VK_RCONTROL)) m |= EVENTFLAG_IS_RIGHT;
        break;
    case VK_MENU:
        if (IsKeyDown(VK_LMENU)) m |= EVENTFLAG_IS_LEFT;
        else if (IsKeyDown(VK_RMENU)) m |= EVENTFLAG_IS_RIGHT;
        break;
    case VK_LWIN: m |= EVENTFLAG_IS_LEFT; break;
    case VK_RWIN: m |= EVENTFLAG_IS_RIGHT; break;
    }
    return m;
}

// --- Key-code translation ---------------------------------------------------

KeyCode vk_to_keycode(int vk) {
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (vk - 'A'));
    if (vk >= '0' && vk <= '9')
        return static_cast<KeyCode>(static_cast<int>(KeyCode::Digit0) + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12)
        return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + (vk - VK_F1));

    switch (vk) {
    case VK_LEFT:    return KeyCode::ArrowLeft;
    case VK_UP:      return KeyCode::ArrowUp;
    case VK_RIGHT:   return KeyCode::ArrowRight;
    case VK_DOWN:    return KeyCode::ArrowDown;
    case VK_HOME:    return KeyCode::Home;
    case VK_END:     return KeyCode::End;
    case VK_PRIOR:   return KeyCode::PageUp;
    case VK_NEXT:    return KeyCode::PageDown;
    case VK_TAB:     return KeyCode::Tab;
    case VK_RETURN:  return KeyCode::Return;
    case VK_ESCAPE:  return KeyCode::Escape;
    case VK_BACK:    return KeyCode::Backspace;
    case VK_DELETE:  return KeyCode::Delete;
    case VK_SPACE:   return KeyCode::Space;
    case VK_INSERT:  return KeyCode::Insert;
    case VK_SHIFT:   return KeyCode::Shift;
    case VK_CONTROL: return KeyCode::Control;
    case VK_MENU:    return KeyCode::Alt;
    case VK_LWIN:
    case VK_RWIN:    return KeyCode::Meta;
    case VK_CAPITAL: return KeyCode::CapsLock;
    default:         return KeyCode::Unknown;
    }
}

// --- Mouse button helpers ---------------------------------------------------

MouseButton msg_to_button(UINT msg) {
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return MouseButton::Left;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return MouseButton::Right;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return MouseButton::Middle;
    default: return MouseButton::Left;
    }
}

bool is_button_up(UINT msg) {
    return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP;
}

bool is_button_down(UINT msg) {
    return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
}

// --- Cursor mapping ---------------------------------------------------------

LPCTSTR cef_cursor_to_win(cef_cursor_type_t type) {
    switch (type) {
    case CT_CROSS:                      return IDC_CROSS;
    case CT_HAND:                       return IDC_HAND;
    case CT_IBEAM:                      return IDC_IBEAM;
    case CT_WAIT:                       return IDC_WAIT;
    case CT_HELP:                       return IDC_HELP;
    case CT_EASTRESIZE:                 return IDC_SIZEWE;
    case CT_NORTHRESIZE:                return IDC_SIZENS;
    case CT_NORTHEASTRESIZE:            return IDC_SIZENESW;
    case CT_NORTHWESTRESIZE:            return IDC_SIZENWSE;
    case CT_SOUTHRESIZE:                return IDC_SIZENS;
    case CT_SOUTHEASTRESIZE:            return IDC_SIZENWSE;
    case CT_SOUTHWESTRESIZE:            return IDC_SIZENESW;
    case CT_WESTRESIZE:                 return IDC_SIZEWE;
    case CT_NORTHSOUTHRESIZE:           return IDC_SIZENS;
    case CT_EASTWESTRESIZE:             return IDC_SIZEWE;
    case CT_NORTHEASTSOUTHWESTRESIZE:   return IDC_SIZENESW;
    case CT_NORTHWESTSOUTHEASTRESIZE:   return IDC_SIZENWSE;
    case CT_COLUMNRESIZE:               return IDC_SIZEWE;
    case CT_ROWRESIZE:                  return IDC_SIZENS;
    case CT_MOVE:                       return IDC_SIZEALL;
    case CT_PROGRESS:                   return IDC_APPSTARTING;
    case CT_NODROP:                     return IDC_NO;
    case CT_NOTALLOWED:                 return IDC_NO;
    case CT_GRAB:                       return IDC_HAND;
    case CT_GRABBING:                   return IDC_HAND;
    case CT_MIDDLEPANNING:
    case CT_MIDDLE_PANNING_VERTICAL:
    case CT_MIDDLE_PANNING_HORIZONTAL:  return IDC_SIZEALL;
    default:                            return IDC_ARROW;
    }
}

// --- Input WndProc ----------------------------------------------------------

LRESULT CALLBACK input_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (g.cursor_type == CT_NONE)
                SetCursor(nullptr);
            else
                SetCursor(LoadCursor(nullptr, cef_cursor_to_win(g.cursor_type)));
            return TRUE;
        }
        break;

    // --- Mouse move ---
    case WM_MOUSEMOVE:
        dispatch_mouse_move({
            .x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp),
            .modifiers = mouse_modifiers(wp),
            .leave = false,
        });
        return 0;

    case WM_MOUSELEAVE:
        dispatch_mouse_move({
            .x = -1, .y = -1,
            .modifiers = mouse_modifiers(wp),
            .leave = true,
        });
        return 0;

    // --- Mouse buttons ---
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:   case WM_RBUTTONUP:   case WM_MBUTTONUP: {
        LOG_TRACE(LOG_PLATFORM, "[INPUT] wm_button msg=0x{:x} down={}", msg, is_button_down(msg));
        if (is_button_down(msg)) SetFocus(hwnd);
        dispatch_mouse_button({
            .button      = msg_to_button(msg),
            .pressed     = is_button_down(msg),
            .x           = GET_X_LPARAM(lp),
            .y           = GET_Y_LPARAM(lp),
            .click_count = 1,
            .modifiers   = mouse_modifiers(wp),
        });
        return 0;
    }

    case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
        WORD btn = GET_XBUTTON_WPARAM(wp);
        LOG_TRACE(LOG_PLATFORM, "[INPUT] wm_xbutton msg=0x{:x} btn={}", msg, btn);
        if (msg == WM_XBUTTONDOWN) {
            // XBUTTON1 = mouse "back", XBUTTON2 = "forward"
            dispatch_history_nav(btn == XBUTTON2);
        }
        return TRUE;  // must return TRUE for WM_XBUTTON* per MSDN
    }

    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        dispatch_scroll({
            .x = pt.x, .y = pt.y,
            .dx = 0,   .dy = GET_WHEEL_DELTA_WPARAM(wp),
            .modifiers = mouse_modifiers(wp),
        });
        return 0;
    }

    case WM_MOUSEHWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        dispatch_scroll({
            .x = pt.x, .y = pt.y,
            .dx = GET_WHEEL_DELTA_WPARAM(wp), .dy = 0,
            .modifiers = mouse_modifiers(wp),
        });
        return 0;
    }

    // --- Keyboard ---
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP:   case WM_SYSKEYUP: {
        // Alt+F4: initiate shutdown (child HWNDs don't get WM_CLOSE from DefWindowProc)
        if (wp == VK_F4 && msg == WM_SYSKEYDOWN && IsKeyDown(VK_MENU)) {
            PostMessage(g.mpv_hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        KeyEvent e{};
        e.code             = vk_to_keycode(static_cast<int>(wp));
        e.windows_key_code = static_cast<int>(wp);
        e.action           = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
                                 ? KeyAction::Down : KeyAction::Up;
        e.modifiers        = keyboard_modifiers(wp, lp);
        e.native_key_code  = static_cast<int>(lp);
        e.is_system_key    = (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP);
        dispatch_key(e);
        return 0;
    }

    case WM_CHAR: case WM_SYSCHAR: {
        dispatch_char(static_cast<uint32_t>(wp),
                      keyboard_modifiers(wp, lp),
                      static_cast<int>(lp),
                      msg == WM_SYSCHAR);
        return 0;
    }

    // --- Focus ---
    case WM_SETFOCUS:
        dispatch_keyboard_focus(true);
        return 0;
    case WM_KILLFOCUS:
        dispatch_keyboard_focus(false);
        return 0;

    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

}  // namespace

// --- Public API -------------------------------------------------------------

void run_input_thread(HWND mpv_hwnd) {
    g.mpv_hwnd  = mpv_hwnd;
    g.thread_id = GetCurrentThreadId();

    WNDCLASSEXW wc = {};
    wc.cbSize       = sizeof(wc);
    wc.lpfnWndProc  = input_wndproc;
    wc.hInstance    = GetModuleHandle(nullptr);
    wc.lpszClassName = L"JellyfinCefInput";
    wc.style        = 0;
    wc.hCursor      = nullptr;  // managed via WM_SETCURSOR
    RegisterClassExW(&wc);

    RECT rc;
    GetClientRect(mpv_hwnd, &rc);

    g.input_hwnd = CreateWindowExW(
        0, L"JellyfinCefInput", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        mpv_hwnd, nullptr, GetModuleHandle(nullptr), nullptr);

    DWORD mpv_tid = GetWindowThreadProcessId(mpv_hwnd, nullptr);
    AttachThreadInput(g.thread_id, mpv_tid, TRUE);
    SetFocus(g.input_hwnd);

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    if (g.input_hwnd) { DestroyWindow(g.input_hwnd); g.input_hwnd = nullptr; }
    UnregisterClassW(L"JellyfinCefInput", GetModuleHandle(nullptr));
    g.thread_id = 0;
}

void stop_input_thread() {
    if (g.thread_id)
        PostThreadMessage(g.thread_id, WM_QUIT, 0, 0);
}

void resize_to_parent(int pw, int ph) {
    if (g.input_hwnd)
        SetWindowPos(g.input_hwnd, nullptr, 0, 0, pw, ph,
                     SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

void set_cursor(cef_cursor_type_t type) {
    g.cursor_type = type;
    // SetCursor is thread-affine — must run on the input thread.
    // Post a synthetic WM_SETCURSOR so our WndProc applies it immediately.
    if (g.input_hwnd)
        PostMessage(g.input_hwnd, WM_SETCURSOR, (WPARAM)g.input_hwnd,
                    MAKELPARAM(HTCLIENT, 0));
}

}  // namespace input::windows
