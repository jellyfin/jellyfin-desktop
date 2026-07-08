//! Windows input — Win32 child window owning all keyboard/mouse for CEF.
//!
//! Runs on a dedicated thread (spawned by `platform.rs::win_init`);
//! registers a `JellyfinCefInput` window class, creates a child of mpv's
//! HWND covering the client area, and translates `WM_*` messages into
//! the platform-agnostic `jfn_input_dispatch_*` entry points exposed by
//! `src/input/src/lib.rs`.

#![allow(non_snake_case)]

use parking_lot::Mutex;
use std::ffi::c_int;

use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::ScreenToClient;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::SystemServices::{
    APPCOMMAND_BROWSER_BACKWARD, APPCOMMAND_BROWSER_FORWARD, MK_CONTROL, MK_LBUTTON, MK_MBUTTON,
    MK_RBUTTON, MK_SHIFT,
};
use windows::Win32::System::Threading::{AttachThreadInput, GetCurrentThreadId};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetKeyState, SetFocus, VK_ADD, VK_BROWSER_BACK, VK_BROWSER_FORWARD, VK_CAPITAL, VK_CLEAR,
    VK_CONTROL, VK_DECIMAL, VK_DELETE, VK_DIVIDE, VK_DOWN, VK_END, VK_F4, VK_HOME, VK_INSERT,
    VK_LCONTROL, VK_LEFT, VK_LMENU, VK_LSHIFT, VK_LWIN, VK_MENU, VK_MULTIPLY, VK_NEXT, VK_NUMLOCK,
    VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7,
    VK_NUMPAD8, VK_NUMPAD9, VK_PRIOR, VK_RCONTROL, VK_RETURN, VK_RIGHT, VK_RMENU, VK_RSHIFT,
    VK_RWIN, VK_SHIFT, VK_SUBTRACT, VK_UP,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, GetClientRect, GetMessageW,
    GetWindowThreadProcessId, HCURSOR, HICON, HMENU, HTCLIENT, IDC_APPSTARTING, IDC_ARROW,
    IDC_CROSS, IDC_HAND, IDC_HELP, IDC_IBEAM, IDC_NO, IDC_SIZEALL, IDC_SIZENESW, IDC_SIZENS,
    IDC_SIZENWSE, IDC_SIZEWE, IDC_WAIT, KF_EXTENDED, LoadCursorW, MSG, PostMessageW,
    PostThreadMessageW, RegisterClassExW, SET_WINDOW_POS_FLAGS, SWP_NOACTIVATE, SWP_NOMOVE,
    SWP_NOZORDER, SetCursor, SetWindowPos, TranslateMessage, UnregisterClassW, WINDOW_EX_STYLE,
    WINDOW_STYLE, WM_APPCOMMAND, WM_CHAR, WM_KEYDOWN, WM_KEYUP, WM_KILLFOCUS, WM_LBUTTONDBLCLK,
    WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MBUTTONDBLCLK, WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MOUSEHWHEEL,
    WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_QUIT, WM_RBUTTONDBLCLK, WM_RBUTTONDOWN, WM_RBUTTONUP,
    WM_SETCURSOR, WM_SETFOCUS, WM_SYSCHAR, WM_SYSKEYDOWN, WM_SYSKEYUP, WM_XBUTTONDOWN,
    WM_XBUTTONUP, WNDCLASSEXW, WS_CHILD, WS_VISIBLE, XBUTTON2,
};
use windows::core::{PCWSTR, w};

// Not re-exported by windows-rs 0.62's WindowsAndMessaging metadata.
const WM_MOUSELEAVE: u32 = 0x02A3;

// =====================================================================
// CEF cursor-type ordinals + event flags (mirrors cef_types.h)
// =====================================================================

use jfn_input::buttons::{BTN_LEFT, BTN_MIDDLE, BTN_RIGHT};
use jfn_platform_abi::cursor::CursorShape;
use jfn_platform_abi::event_flags::{
    EVENTFLAG_ALT_DOWN, EVENTFLAG_CAPS_LOCK_ON, EVENTFLAG_CONTROL_DOWN, EVENTFLAG_IS_KEY_PAD,
    EVENTFLAG_IS_LEFT, EVENTFLAG_IS_RIGHT, EVENTFLAG_LEFT_MOUSE_BUTTON,
    EVENTFLAG_MIDDLE_MOUSE_BUTTON, EVENTFLAG_NUM_LOCK_ON, EVENTFLAG_RIGHT_MOUSE_BUTTON,
    EVENTFLAG_SHIFT_DOWN,
};

use jfn_input::{
    jfn_input_dispatch_char_sys, jfn_input_dispatch_history_nav, jfn_input_dispatch_key_full,
    jfn_input_dispatch_keyboard_focus, jfn_input_dispatch_mouse_button,
    jfn_input_dispatch_mouse_move, jfn_input_dispatch_scroll,
};
use jfn_playback::ingest_driver::jfn_playback_display_scale;
use jfn_playback::shutdown::jfn_shutdown_initiate;

// =====================================================================
// Shared state. `set_cursor` is invoked from the CEF UI thread; the
// input thread reads `cursor_type` from WM_SETCURSOR. `input_hwnd_raw`
// and `thread_id` are written once during run_input_thread startup and
// read by the cross-thread set_cursor / stop / resize helpers.
// =====================================================================

struct State {
    input_hwnd_raw: usize,
    thread_id: u32,
    cursor_type: i32,
}

static STATE: Mutex<State> = Mutex::new(State {
    input_hwnd_raw: 0,
    thread_id: 0,
    cursor_type: CursorShape::Pointer.as_raw(),
});

// =====================================================================
// Win32 macro helpers — windows-rs doesn't ship the *_LPARAM / *_WPARAM
// macros from Windows headers, so reimplement the ones we need inline.
// =====================================================================

#[inline]
fn loword_u32(v: u32) -> u16 {
    (v & 0xFFFF) as u16
}

#[inline]
fn hiword_i16(v: u32) -> i16 {
    ((v >> 16) & 0xFFFF) as i16
}

#[inline]
fn get_x_lparam(lp: LPARAM) -> i32 {
    (lp.0 as i16) as i32
}
#[inline]
fn get_y_lparam(lp: LPARAM) -> i32 {
    ((lp.0 >> 16) as i16) as i32
}

#[inline]
fn get_xbutton_wparam(wp: WPARAM) -> u16 {
    hiword_i16(wp.0 as u32) as u16
}

#[inline]
fn get_appcommand_lparam(lp: LPARAM) -> u16 {
    (hiword_i16(lp.0 as u32) as u16) & 0x7FFF
}

// =====================================================================
// Modifier helpers.
// =====================================================================

#[inline]
fn is_key_down(vk: u16) -> bool {
    let s = unsafe { GetKeyState(vk as i32) };
    (s as u16 & 0x8000) != 0
}

fn mouse_modifiers(wp: WPARAM) -> u32 {
    let mut m = 0u32;
    let w = wp.0 as u32;
    if w & MK_CONTROL.0 != 0 {
        m |= EVENTFLAG_CONTROL_DOWN;
    }
    if w & MK_SHIFT.0 != 0 {
        m |= EVENTFLAG_SHIFT_DOWN;
    }
    if is_key_down(VK_MENU.0) {
        m |= EVENTFLAG_ALT_DOWN;
    }
    if w & MK_LBUTTON.0 != 0 {
        m |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    }
    if w & MK_RBUTTON.0 != 0 {
        m |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    }
    if w & MK_MBUTTON.0 != 0 {
        m |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    }
    m
}

fn keyboard_modifiers(wp: WPARAM, lp: LPARAM) -> u32 {
    let mut m = 0u32;
    if is_key_down(VK_SHIFT.0) {
        m |= EVENTFLAG_SHIFT_DOWN;
    }
    if is_key_down(VK_CONTROL.0) {
        m |= EVENTFLAG_CONTROL_DOWN;
    }
    if is_key_down(VK_MENU.0) {
        m |= EVENTFLAG_ALT_DOWN;
    }
    if (unsafe { GetKeyState(VK_NUMLOCK.0 as i32) } & 1) != 0 {
        m |= EVENTFLAG_NUM_LOCK_ON;
    }
    if (unsafe { GetKeyState(VK_CAPITAL.0 as i32) } & 1) != 0 {
        m |= EVENTFLAG_CAPS_LOCK_ON;
    }

    let extended = ((lp.0 >> 16) as u32 & KF_EXTENDED) != 0;
    let vk = wp.0 as u16;
    match vk {
        v if v == VK_RETURN.0 && extended => {
            m |= EVENTFLAG_IS_KEY_PAD;
        }
        v if !extended
            && (v == VK_INSERT.0
                || v == VK_DELETE.0
                || v == VK_HOME.0
                || v == VK_END.0
                || v == VK_PRIOR.0
                || v == VK_NEXT.0
                || v == VK_UP.0
                || v == VK_DOWN.0
                || v == VK_LEFT.0
                || v == VK_RIGHT.0) =>
        {
            m |= EVENTFLAG_IS_KEY_PAD;
        }
        v if v == VK_NUMLOCK.0
            || v == VK_NUMPAD0.0
            || v == VK_NUMPAD1.0
            || v == VK_NUMPAD2.0
            || v == VK_NUMPAD3.0
            || v == VK_NUMPAD4.0
            || v == VK_NUMPAD5.0
            || v == VK_NUMPAD6.0
            || v == VK_NUMPAD7.0
            || v == VK_NUMPAD8.0
            || v == VK_NUMPAD9.0
            || v == VK_DIVIDE.0
            || v == VK_MULTIPLY.0
            || v == VK_SUBTRACT.0
            || v == VK_ADD.0
            || v == VK_DECIMAL.0
            || v == VK_CLEAR.0 =>
        {
            m |= EVENTFLAG_IS_KEY_PAD;
        }
        v if v == VK_SHIFT.0 => {
            if is_key_down(VK_LSHIFT.0) {
                m |= EVENTFLAG_IS_LEFT;
            } else if is_key_down(VK_RSHIFT.0) {
                m |= EVENTFLAG_IS_RIGHT;
            }
        }
        v if v == VK_CONTROL.0 => {
            if is_key_down(VK_LCONTROL.0) {
                m |= EVENTFLAG_IS_LEFT;
            } else if is_key_down(VK_RCONTROL.0) {
                m |= EVENTFLAG_IS_RIGHT;
            }
        }
        v if v == VK_MENU.0 => {
            if is_key_down(VK_LMENU.0) {
                m |= EVENTFLAG_IS_LEFT;
            } else if is_key_down(VK_RMENU.0) {
                m |= EVENTFLAG_IS_RIGHT;
            }
        }
        v if v == VK_LWIN.0 => m |= EVENTFLAG_IS_LEFT,
        v if v == VK_RWIN.0 => m |= EVENTFLAG_IS_RIGHT,
        _ => {}
    }
    m
}

// =====================================================================
// Cursor mapping.
// =====================================================================

fn cef_cursor_to_win(shape: CursorShape) -> PCWSTR {
    use CursorShape::*;
    match shape {
        Cross => IDC_CROSS,
        Hand | Grab | Grabbing => IDC_HAND,
        IBeam => IDC_IBEAM,
        Wait => IDC_WAIT,
        Help => IDC_HELP,
        EastResize | WestResize | EastWestResize | ColumnResize => IDC_SIZEWE,
        NorthResize | SouthResize | NorthSouthResize | RowResize => IDC_SIZENS,
        NorthEastResize | SouthWestResize | NorthEastSouthWestResize => IDC_SIZENESW,
        NorthWestResize | SouthEastResize | NorthWestSouthEastResize => IDC_SIZENWSE,
        Move | MiddlePanning | MiddlePanningVertical | MiddlePanningHorizontal => IDC_SIZEALL,
        Progress => IDC_APPSTARTING,
        NoDrop | NotAllowed => IDC_NO,
        _ => IDC_ARROW,
    }
}

// =====================================================================
// Mouse button helpers.
// =====================================================================

fn msg_to_button_code(msg: u32) -> u32 {
    match msg {
        WM_LBUTTONDOWN | WM_LBUTTONUP | WM_LBUTTONDBLCLK => BTN_LEFT,
        WM_RBUTTONDOWN | WM_RBUTTONUP | WM_RBUTTONDBLCLK => BTN_RIGHT,
        WM_MBUTTONDOWN | WM_MBUTTONUP | WM_MBUTTONDBLCLK => BTN_MIDDLE,
        _ => BTN_LEFT,
    }
}

#[inline]
fn is_button_down(msg: u32) -> bool {
    matches!(msg, WM_LBUTTONDOWN | WM_RBUTTONDOWN | WM_MBUTTONDOWN)
}

fn to_logical(physical: i32) -> i32 {
    let scale = jfn_playback_display_scale();
    let s = if scale > 0.0 { scale } else { 1.0 };
    (physical as f64 / s) as i32
}

// =====================================================================
// WndProc.
// =====================================================================

unsafe extern "system" fn input_wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_SETCURSOR if u32::from(loword_u32(lp.0 as u32)) == HTCLIENT => {
            let shape =
                CursorShape::from_cef(STATE.lock().cursor_type).unwrap_or(CursorShape::Pointer);
            if shape == CursorShape::None {
                unsafe { SetCursor(None) };
            } else {
                let cur = unsafe { LoadCursorW(None, cef_cursor_to_win(shape)).ok() };
                unsafe { SetCursor(cur) };
            }
            return LRESULT(1);
        }

        WM_MOUSEMOVE => {
            jfn_input_dispatch_mouse_move(
                to_logical(get_x_lparam(lp)),
                to_logical(get_y_lparam(lp)),
                mouse_modifiers(wp),
                0,
            );
            return LRESULT(0);
        }

        WM_MOUSELEAVE => {
            jfn_input_dispatch_mouse_move(-1, -1, mouse_modifiers(wp), 1);
            return LRESULT(0);
        }

        WM_LBUTTONDOWN | WM_RBUTTONDOWN | WM_MBUTTONDOWN | WM_LBUTTONUP | WM_RBUTTONUP
        | WM_MBUTTONUP => {
            let down = is_button_down(msg);
            if down {
                let _ = unsafe { SetFocus(Some(hwnd)) };
            }
            jfn_input_dispatch_mouse_button(
                msg_to_button_code(msg),
                if down { 1 } else { 0 },
                to_logical(get_x_lparam(lp)),
                to_logical(get_y_lparam(lp)),
                mouse_modifiers(wp),
            );
            return LRESULT(0);
        }

        WM_XBUTTONDOWN | WM_XBUTTONUP => {
            let btn = get_xbutton_wparam(wp);
            if msg == WM_XBUTTONDOWN {
                let fwd = if btn == XBUTTON2 { 1 } else { 0 };
                jfn_input_dispatch_history_nav(fwd);
            }
            return LRESULT(1); // TRUE per MSDN
        }

        WM_APPCOMMAND => {
            let cmd = get_appcommand_lparam(lp) as u32;
            if cmd == APPCOMMAND_BROWSER_BACKWARD.0 {
                jfn_input_dispatch_history_nav(0);
                return LRESULT(1);
            }
            if cmd == APPCOMMAND_BROWSER_FORWARD.0 {
                jfn_input_dispatch_history_nav(1);
                return LRESULT(1);
            }
            // bubble unhandled commands to parent via DefWindowProc.
        }

        WM_MOUSEWHEEL => {
            let mut pt = POINT {
                x: get_x_lparam(lp),
                y: get_y_lparam(lp),
            };
            unsafe {
                let _ = ScreenToClient(hwnd, &mut pt);
            }
            let delta = hiword_i16(wp.0 as u32) as i32;
            jfn_input_dispatch_scroll(
                to_logical(pt.x),
                to_logical(pt.y),
                0,
                delta,
                mouse_modifiers(wp),
            );
            return LRESULT(0);
        }

        WM_MOUSEHWHEEL => {
            let mut pt = POINT {
                x: get_x_lparam(lp),
                y: get_y_lparam(lp),
            };
            unsafe {
                let _ = ScreenToClient(hwnd, &mut pt);
            }
            let delta = hiword_i16(wp.0 as u32) as i32;
            jfn_input_dispatch_scroll(
                to_logical(pt.x),
                to_logical(pt.y),
                delta,
                0,
                mouse_modifiers(wp),
            );
            return LRESULT(0);
        }

        WM_KEYDOWN | WM_SYSKEYDOWN | WM_KEYUP | WM_SYSKEYUP => {
            let vk = wp.0 as u16;
            if vk == VK_F4.0 && msg == WM_SYSKEYDOWN && is_key_down(VK_MENU.0) {
                jfn_shutdown_initiate();
                return LRESULT(0);
            }
            // Browser nav keystrokes (some IR drivers).
            if vk == VK_BROWSER_BACK.0 || vk == VK_BROWSER_FORWARD.0 {
                if msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN {
                    let fwd = if vk == VK_BROWSER_FORWARD.0 { 1 } else { 0 };
                    jfn_input_dispatch_history_nav(fwd);
                }
                return LRESULT(0);
            }
            let pressed = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            let is_sys = msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
            jfn_input_dispatch_key_full(
                if pressed { 1 } else { 0 },
                vk as i32,
                lp.0 as i32,
                keyboard_modifiers(wp, lp),
                0,
                0,
                if is_sys { 1 } else { 0 },
            );
            return LRESULT(0);
        }

        WM_CHAR | WM_SYSCHAR => {
            jfn_input_dispatch_char_sys(
                wp.0 as u32,
                keyboard_modifiers(wp, lp),
                lp.0 as u32,
                if msg == WM_SYSCHAR { 1 } else { 0 },
            );
            return LRESULT(0);
        }

        WM_SETFOCUS => {
            jfn_input_dispatch_keyboard_focus(1);
            return LRESULT(0);
        }
        WM_KILLFOCUS => {
            jfn_input_dispatch_keyboard_focus(0);
            return LRESULT(0);
        }

        _ => {}
    }
    unsafe { DefWindowProcW(hwnd, msg, wp, lp) }
}

// =====================================================================
// Thread entry — registers the class, creates the child window, runs
// the message loop, then cleans up. Called from std::thread::spawn in
// platform.rs::win_init.
// =====================================================================

const CLASS_NAME: PCWSTR = w!("JellyfinCefInput");

pub fn jfn_input_windows_run_input_thread(mpv_hwnd: *mut std::ffi::c_void) {
    let mpv = HWND(mpv_hwnd);
    let tid = unsafe { GetCurrentThreadId() };

    {
        let mut s = STATE.lock();
        s.thread_id = tid;
    }

    let hinst = unsafe { GetModuleHandleW(None).unwrap_or_default() };

    let wc = WNDCLASSEXW {
        cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
        style: Default::default(),
        lpfnWndProc: Some(input_wndproc),
        cbClsExtra: 0,
        cbWndExtra: 0,
        hInstance: hinst.into(),
        hIcon: HICON::default(),
        // No class cursor — WM_SETCURSOR drives it.
        hCursor: HCURSOR::default(),
        hbrBackground: Default::default(),
        lpszMenuName: PCWSTR::null(),
        lpszClassName: CLASS_NAME,
        hIconSm: HICON::default(),
    };
    unsafe { RegisterClassExW(&wc) };

    let mut rc = RECT::default();
    let _ = unsafe { GetClientRect(mpv, &mut rc) };

    let input_hwnd = unsafe {
        CreateWindowExW(
            WINDOW_EX_STYLE(0),
            CLASS_NAME,
            w!(""),
            WINDOW_STYLE(WS_CHILD.0 | WS_VISIBLE.0),
            0,
            0,
            rc.right - rc.left,
            rc.bottom - rc.top,
            Some(mpv),
            Some(HMENU(std::ptr::null_mut())),
            Some(hinst.into()),
            None,
        )
    };

    let input_hwnd = match input_hwnd {
        Ok(h) => h,
        Err(e) => {
            tracing::error!("CreateWindowExW(JellyfinCefInput) failed: {e:?}");
            STATE.lock().thread_id = 0;
            return;
        }
    };
    STATE.lock().input_hwnd_raw = input_hwnd.0 as usize;

    // Share input queue with mpv so SetFocus across windows works.
    let mpv_tid = unsafe { GetWindowThreadProcessId(mpv, None) };
    let _ = unsafe { AttachThreadInput(tid, mpv_tid, true) };
    let _ = unsafe { SetFocus(Some(input_hwnd)) };

    // Standard GetMessage/Dispatch loop.
    let mut m = MSG::default();
    while unsafe { GetMessageW(&mut m, None, 0, 0).0 } > 0 {
        unsafe {
            let _ = TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if STATE.lock().input_hwnd_raw != 0 {
        let _ = unsafe { DestroyWindow(input_hwnd) };
        STATE.lock().input_hwnd_raw = 0;
    }
    let _ = unsafe { UnregisterClassW(CLASS_NAME, Some(hinst.into())) };
    STATE.lock().thread_id = 0;
}

pub fn jfn_input_windows_stop_input_thread() {
    let tid = STATE.lock().thread_id;
    if tid != 0 {
        let _ = unsafe { PostThreadMessageW(tid, WM_QUIT, WPARAM(0), LPARAM(0)) };
    }
}

pub fn jfn_input_windows_resize_to_parent(pw: c_int, ph: c_int) {
    let hwnd_raw = STATE.lock().input_hwnd_raw;
    if hwnd_raw == 0 {
        return;
    }
    let hwnd = HWND(hwnd_raw as *mut _);
    let flags: SET_WINDOW_POS_FLAGS =
        SET_WINDOW_POS_FLAGS(SWP_NOZORDER.0 | SWP_NOMOVE.0 | SWP_NOACTIVATE.0);
    let _ = unsafe { SetWindowPos(hwnd, None, 0, 0, pw, ph, flags) };
}

/// Platform::set_cursor — invoked from the CEF UI thread. Stores the
/// pending cursor type and posts a synthetic WM_SETCURSOR so the input
/// thread applies it via SetCursor (which is thread-affine).
pub fn jfn_input_windows_set_cursor(t: c_int) {
    let hwnd_raw = {
        let mut s = STATE.lock();
        s.cursor_type = t;
        s.input_hwnd_raw
    };
    if hwnd_raw == 0 {
        return;
    }
    let hwnd = HWND(hwnd_raw as *mut _);
    // wparam = hwnd, lparam = MAKELPARAM(HTCLIENT=1, 0)
    let _ = unsafe { PostMessageW(Some(hwnd), WM_SETCURSOR, WPARAM(hwnd_raw), LPARAM(1)) };
}
