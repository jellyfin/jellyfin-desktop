//! Windows input — Win32 child window owning all keyboard/mouse for CEF.
//!
//! Ported from `src/input/input_windows.cpp`. Runs on a dedicated thread
//! (spawned by `platform.rs::win_init`); registers a `JellyfinCefInput`
//! window class, creates a child of mpv's HWND covering the client area,
//! and translates `WM_*` messages into the platform-agnostic
//! `jfn_input_dispatch_*` C ABI exposed by `src/input/src/lib.rs`.

#![allow(non_snake_case)]

use std::ffi::c_int;
use std::sync::Mutex;

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
    VK_SHIFT, VK_SUBTRACT, VK_UP,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, GetClientRect, GetMessageW,
    GetWindowThreadProcessId, HCURSOR, HICON, HMENU, IDC_APPSTARTING, IDC_ARROW, IDC_CROSS,
    IDC_HAND, IDC_HELP, IDC_IBEAM, IDC_NO, IDC_SIZEALL, IDC_SIZENESW, IDC_SIZENS, IDC_SIZENWSE,
    IDC_SIZEWE, IDC_WAIT, KF_EXTENDED, LoadCursorW, MSG, PostMessageW, PostThreadMessageW,
    RegisterClassExW, SET_WINDOW_POS_FLAGS, SWP_NOACTIVATE, SWP_NOMOVE, SWP_NOZORDER, SetCursor,
    SetWindowPos, TranslateMessage, UnregisterClassW, WINDOW_EX_STYLE, WINDOW_STYLE,
    WM_APPCOMMAND, WM_CHAR, WM_CLOSE, WM_KEYDOWN, WM_KEYUP, WM_KILLFOCUS, WM_LBUTTONDBLCLK,
    WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MBUTTONDBLCLK, WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MOUSEHWHEEL,
    WM_MOUSELEAVE, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_QUIT, WM_RBUTTONDBLCLK, WM_RBUTTONDOWN,
    WM_RBUTTONUP, WM_SETCURSOR, WM_SETFOCUS, WM_SYSCHAR, WM_SYSKEYDOWN, WM_SYSKEYUP,
    WM_XBUTTONDOWN, WM_XBUTTONUP, WNDCLASSEXW, WS_CHILD, WS_VISIBLE, XBUTTON2,
};
use windows::core::{PCWSTR, w};

// =====================================================================
// CEF cursor-type ordinals + event flags (mirrors cef_types.h)
// =====================================================================

const CT_POINTER: i32 = 0;
const CT_CROSS: i32 = 1;
const CT_HAND: i32 = 2;
const CT_IBEAM: i32 = 3;
const CT_WAIT: i32 = 4;
const CT_HELP: i32 = 5;
const CT_EASTRESIZE: i32 = 6;
const CT_NORTHRESIZE: i32 = 7;
const CT_NORTHEASTRESIZE: i32 = 8;
const CT_NORTHWESTRESIZE: i32 = 9;
const CT_SOUTHRESIZE: i32 = 10;
const CT_SOUTHEASTRESIZE: i32 = 11;
const CT_SOUTHWESTRESIZE: i32 = 12;
const CT_WESTRESIZE: i32 = 13;
const CT_NORTHSOUTHRESIZE: i32 = 14;
const CT_EASTWESTRESIZE: i32 = 15;
const CT_NORTHEASTSOUTHWESTRESIZE: i32 = 16;
const CT_NORTHWESTSOUTHEASTRESIZE: i32 = 17;
const CT_COLUMNRESIZE: i32 = 18;
const CT_ROWRESIZE: i32 = 19;
const CT_MIDDLEPANNING: i32 = 20;
const CT_MIDDLE_PANNING_VERTICAL: i32 = 21;
const CT_MIDDLE_PANNING_HORIZONTAL: i32 = 22;
const CT_PROGRESS: i32 = 26;
const CT_MOVE: i32 = 29;
const CT_NODROP: i32 = 35;
const CT_NONE: i32 = 37;
const CT_NOTALLOWED: i32 = 38;
const CT_GRAB: i32 = 41;
const CT_GRABBING: i32 = 42;

const EVENTFLAG_CAPS_LOCK_ON: u32 = 1 << 0;
const EVENTFLAG_SHIFT_DOWN: u32 = 1 << 1;
const EVENTFLAG_CONTROL_DOWN: u32 = 1 << 2;
const EVENTFLAG_ALT_DOWN: u32 = 1 << 3;
const EVENTFLAG_LEFT_MOUSE_BUTTON: u32 = 1 << 4;
const EVENTFLAG_MIDDLE_MOUSE_BUTTON: u32 = 1 << 5;
const EVENTFLAG_RIGHT_MOUSE_BUTTON: u32 = 1 << 6;
const EVENTFLAG_NUM_LOCK_ON: u32 = 1 << 8;
const EVENTFLAG_IS_KEY_PAD: u32 = 1 << 9;
const EVENTFLAG_IS_LEFT: u32 = 1 << 10;
const EVENTFLAG_IS_RIGHT: u32 = 1 << 11;

// Mouse button codes (matching input::dispatch.h's encoding).
const MOUSE_BTN_LEFT: u32 = 0x110;
const MOUSE_BTN_RIGHT: u32 = 0x111;
const MOUSE_BTN_MIDDLE: u32 = 0x112;

// =====================================================================
// Dispatch entry points implemented in src/input/src/lib.rs.
// =====================================================================

unsafe extern "C" {
    fn jfn_input_dispatch_mouse_move(x: i32, y: i32, mods: u32, leave: c_int);
    fn jfn_input_dispatch_mouse_button(
        button_code: u32,
        pressed: c_int,
        x: i32,
        y: i32,
        mods: u32,
    );
    fn jfn_input_dispatch_scroll(x: i32, y: i32, dx: i32, dy: i32, mods: u32);
    fn jfn_input_dispatch_history_nav(forward: c_int);
    fn jfn_input_dispatch_keyboard_focus(gained: c_int);
    fn jfn_input_dispatch_char_sys(
        codepoint: u32,
        mods: u32,
        native_code: u32,
        is_system_key: c_int,
    );
    fn jfn_input_dispatch_key_full(
        pressed: c_int,
        windows_key_code: i32,
        native_key_code: i32,
        modifiers: u32,
        character: u16,
        unmodified_character: u16,
        is_system_key: c_int,
    );
}

// =====================================================================
// Shared state. `set_cursor` is invoked from the CEF UI thread; the
// input thread reads `cursor_type` from WM_SETCURSOR. `input_hwnd_raw`
// and `thread_id` are written once during run_input_thread startup and
// read by the cross-thread set_cursor / stop / resize helpers.
// =====================================================================

struct State {
    mpv_hwnd_raw: usize,
    input_hwnd_raw: usize,
    thread_id: u32,
    cursor_type: i32,
}

static STATE: Mutex<State> = Mutex::new(State {
    mpv_hwnd_raw: 0,
    input_hwnd_raw: 0,
    thread_id: 0,
    cursor_type: CT_POINTER,
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
        v if v == VK_RETURN.0 => {
            if extended {
                m |= EVENTFLAG_IS_KEY_PAD;
            }
        }
        v if v == VK_INSERT.0
            || v == VK_DELETE.0
            || v == VK_HOME.0
            || v == VK_END.0
            || v == VK_PRIOR.0
            || v == VK_NEXT.0
            || v == VK_UP.0
            || v == VK_DOWN.0
            || v == VK_LEFT.0
            || v == VK_RIGHT.0 =>
        {
            if !extended {
                m |= EVENTFLAG_IS_KEY_PAD;
            }
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
// Cursor mapping. Match input_windows.cpp's cef_cursor_to_win table.
// =====================================================================

fn cef_cursor_to_win(ct: i32) -> PCWSTR {
    match ct {
        CT_CROSS => IDC_CROSS,
        CT_HAND | CT_GRAB | CT_GRABBING => IDC_HAND,
        CT_IBEAM => IDC_IBEAM,
        CT_WAIT => IDC_WAIT,
        CT_HELP => IDC_HELP,
        CT_EASTRESIZE | CT_WESTRESIZE | CT_EASTWESTRESIZE | CT_COLUMNRESIZE => IDC_SIZEWE,
        CT_NORTHRESIZE | CT_SOUTHRESIZE | CT_NORTHSOUTHRESIZE | CT_ROWRESIZE => IDC_SIZENS,
        CT_NORTHEASTRESIZE | CT_SOUTHWESTRESIZE | CT_NORTHEASTSOUTHWESTRESIZE => IDC_SIZENESW,
        CT_NORTHWESTRESIZE | CT_SOUTHEASTRESIZE | CT_NORTHWESTSOUTHEASTRESIZE => IDC_SIZENWSE,
        CT_MOVE | CT_MIDDLEPANNING | CT_MIDDLE_PANNING_VERTICAL | CT_MIDDLE_PANNING_HORIZONTAL => {
            IDC_SIZEALL
        }
        CT_PROGRESS => IDC_APPSTARTING,
        CT_NODROP | CT_NOTALLOWED => IDC_NO,
        _ => IDC_ARROW,
    }
}

// =====================================================================
// Mouse button helpers.
// =====================================================================

fn msg_to_button_code(msg: u32) -> u32 {
    match msg {
        WM_LBUTTONDOWN | WM_LBUTTONUP | WM_LBUTTONDBLCLK => MOUSE_BTN_LEFT,
        WM_RBUTTONDOWN | WM_RBUTTONUP | WM_RBUTTONDBLCLK => MOUSE_BTN_RIGHT,
        WM_MBUTTONDOWN | WM_MBUTTONUP | WM_MBUTTONDBLCLK => MOUSE_BTN_MIDDLE,
        _ => MOUSE_BTN_LEFT,
    }
}

#[inline]
fn is_button_down(msg: u32) -> bool {
    matches!(msg, WM_LBUTTONDOWN | WM_RBUTTONDOWN | WM_MBUTTONDOWN)
}

// =====================================================================
// WndProc.
// =====================================================================

unsafe extern "system" fn input_wndproc(
    hwnd: HWND,
    msg: u32,
    wp: WPARAM,
    lp: LPARAM,
) -> LRESULT {
    match msg {
        WM_SETCURSOR => {
            // HTCLIENT = 1
            if loword_u32(lp.0 as u32) == 1 {
                let ct = STATE.lock().unwrap().cursor_type;
                if ct == CT_NONE {
                    unsafe { SetCursor(None) };
                } else {
                    let cur = unsafe { LoadCursorW(None, cef_cursor_to_win(ct)).ok() };
                    unsafe { SetCursor(cur) };
                }
                return LRESULT(1); // TRUE
            }
        }

        WM_MOUSEMOVE => {
            unsafe {
                jfn_input_dispatch_mouse_move(
                    get_x_lparam(lp),
                    get_y_lparam(lp),
                    mouse_modifiers(wp),
                    0,
                );
            }
            return LRESULT(0);
        }

        WM_MOUSELEAVE => {
            unsafe {
                jfn_input_dispatch_mouse_move(-1, -1, mouse_modifiers(wp), 1);
            }
            return LRESULT(0);
        }

        WM_LBUTTONDOWN | WM_RBUTTONDOWN | WM_MBUTTONDOWN | WM_LBUTTONUP | WM_RBUTTONUP
        | WM_MBUTTONUP => {
            let down = is_button_down(msg);
            if down {
                let _ = unsafe { SetFocus(Some(hwnd)) };
            }
            unsafe {
                jfn_input_dispatch_mouse_button(
                    msg_to_button_code(msg),
                    if down { 1 } else { 0 },
                    get_x_lparam(lp),
                    get_y_lparam(lp),
                    mouse_modifiers(wp),
                );
            }
            return LRESULT(0);
        }

        WM_XBUTTONDOWN | WM_XBUTTONUP => {
            let btn = get_xbutton_wparam(wp);
            if msg == WM_XBUTTONDOWN {
                let fwd = if btn == XBUTTON2 { 1 } else { 0 };
                unsafe { jfn_input_dispatch_history_nav(fwd) };
            }
            return LRESULT(1); // TRUE per MSDN
        }

        WM_APPCOMMAND => {
            let cmd = get_appcommand_lparam(lp) as u32;
            if cmd == APPCOMMAND_BROWSER_BACKWARD {
                unsafe { jfn_input_dispatch_history_nav(0) };
                return LRESULT(1);
            }
            if cmd == APPCOMMAND_BROWSER_FORWARD {
                unsafe { jfn_input_dispatch_history_nav(1) };
                return LRESULT(1);
            }
            // bubble unhandled commands to parent via DefWindowProc.
        }

        WM_MOUSEWHEEL => {
            let mut pt = POINT { x: get_x_lparam(lp), y: get_y_lparam(lp) };
            unsafe {
                let _ = ScreenToClient(hwnd, &mut pt);
            }
            let delta = hiword_i16(wp.0 as u32) as i32;
            unsafe {
                jfn_input_dispatch_scroll(pt.x, pt.y, 0, delta, mouse_modifiers(wp));
            }
            return LRESULT(0);
        }

        WM_MOUSEHWHEEL => {
            let mut pt = POINT { x: get_x_lparam(lp), y: get_y_lparam(lp) };
            unsafe {
                let _ = ScreenToClient(hwnd, &mut pt);
            }
            let delta = hiword_i16(wp.0 as u32) as i32;
            unsafe {
                jfn_input_dispatch_scroll(pt.x, pt.y, delta, 0, mouse_modifiers(wp));
            }
            return LRESULT(0);
        }

        WM_KEYDOWN | WM_SYSKEYDOWN | WM_KEYUP | WM_SYSKEYUP => {
            let vk = wp.0 as u16;
            // Alt+F4: mpv-hosted child HWND never gets WM_CLOSE from DefWindowProc, so synthesize.
            if vk == VK_F4.0 && msg == WM_SYSKEYDOWN && is_key_down(VK_MENU.0) {
                let mpv_hwnd_raw = STATE.lock().unwrap().mpv_hwnd_raw;
                if mpv_hwnd_raw != 0 {
                    let mpv = HWND(mpv_hwnd_raw as *mut _);
                    let _ = unsafe { PostMessageW(Some(mpv), WM_CLOSE, WPARAM(0), LPARAM(0)) };
                }
                return LRESULT(0);
            }
            // Browser nav keystrokes (some IR drivers).
            if vk == VK_BROWSER_BACK.0 || vk == VK_BROWSER_FORWARD.0 {
                if msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN {
                    let fwd = if vk == VK_BROWSER_FORWARD.0 { 1 } else { 0 };
                    unsafe { jfn_input_dispatch_history_nav(fwd) };
                }
                return LRESULT(0);
            }
            let pressed = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            let is_sys = msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
            unsafe {
                jfn_input_dispatch_key_full(
                    if pressed { 1 } else { 0 },
                    vk as i32,
                    lp.0 as i32,
                    keyboard_modifiers(wp, lp),
                    0,
                    0,
                    if is_sys { 1 } else { 0 },
                );
            }
            return LRESULT(0);
        }

        WM_CHAR | WM_SYSCHAR => {
            unsafe {
                jfn_input_dispatch_char_sys(
                    wp.0 as u32,
                    keyboard_modifiers(wp, lp),
                    lp.0 as u32,
                    if msg == WM_SYSCHAR { 1 } else { 0 },
                );
            }
            return LRESULT(0);
        }

        WM_SETFOCUS => {
            unsafe { jfn_input_dispatch_keyboard_focus(1) };
            return LRESULT(0);
        }
        WM_KILLFOCUS => {
            unsafe { jfn_input_dispatch_keyboard_focus(0) };
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

#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_windows_run_input_thread(mpv_hwnd: *mut std::ffi::c_void) {
    let mpv = HWND(mpv_hwnd);
    let tid = unsafe { GetCurrentThreadId() };

    {
        let mut s = STATE.lock().unwrap();
        s.mpv_hwnd_raw = mpv_hwnd as usize;
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
            STATE.lock().unwrap().thread_id = 0;
            return;
        }
    };
    STATE.lock().unwrap().input_hwnd_raw = input_hwnd.0 as usize;

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

    if STATE.lock().unwrap().input_hwnd_raw != 0 {
        let _ = unsafe { DestroyWindow(input_hwnd) };
        STATE.lock().unwrap().input_hwnd_raw = 0;
    }
    let _ = unsafe { UnregisterClassW(CLASS_NAME, Some(hinst.into())) };
    STATE.lock().unwrap().thread_id = 0;
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_windows_stop_input_thread() {
    let tid = STATE.lock().unwrap().thread_id;
    if tid != 0 {
        let _ = unsafe { PostThreadMessageW(tid, WM_QUIT, WPARAM(0), LPARAM(0)) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_windows_resize_to_parent(pw: c_int, ph: c_int) {
    let hwnd_raw = STATE.lock().unwrap().input_hwnd_raw;
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
#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_windows_set_cursor(t: c_int) {
    let hwnd_raw = {
        let mut s = STATE.lock().unwrap();
        s.cursor_type = t;
        s.input_hwnd_raw
    };
    if hwnd_raw == 0 {
        return;
    }
    let hwnd = HWND(hwnd_raw as *mut _);
    // wparam = hwnd, lparam = MAKELPARAM(HTCLIENT=1, 0)
    let _ = unsafe {
        PostMessageW(
            Some(hwnd),
            WM_SETCURSOR,
            WPARAM(hwnd_raw),
            LPARAM(1),
        )
    };
}
