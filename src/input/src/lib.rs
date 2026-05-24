//! Input dispatch. Translates platform key/pointer events into CEF
//! events and forwards them to the active browser via
//! [`jfn_platform_abi::browser_bridge`].

use jfn_platform_abi::{BrowserBridge, browser_bridge};
use jfn_playback::hotkey::jfn_hotkey_classify_keydown;
use jfn_playback::shutdown::jfn_shutdown_initiate;
use std::os::raw::c_int;
use parking_lot::Mutex;

#[cfg(target_os = "linux")]
mod keysym;

// CEF event-type constants (from include/internal/cef_types.h).
const KEYEVENT_RAWKEYDOWN: c_int = 0;
const KEYEVENT_KEYUP: c_int = 2;
const KEYEVENT_CHAR: c_int = 3;
// CEF mouse-button constants (MBT_*).
const MBT_LEFT: c_int = 0;
const MBT_MIDDLE: c_int = 1;
const MBT_RIGHT: c_int = 2;
// EVENTFLAG_PRECISION_SCROLLING_DELTA.
const EVENTFLAG_PRECISION_SCROLLING_DELTA: u32 = 1 << 17;

#[derive(Copy, Clone, Default)]
struct LastMousePos {
    valid: bool,
    x: i32,
    y: i32,
    modifiers: u32,
}

static LAST_POS: Mutex<LastMousePos> = Mutex::new(LastMousePos {
    valid: false,
    x: 0,
    y: 0,
    modifiers: 0,
});

fn cef_button(button_code: u32) -> Option<c_int> {
    match button_code {
        0x110 => Some(MBT_LEFT),
        0x111 => Some(MBT_RIGHT),
        0x112 => Some(MBT_MIDDLE),
        _ => None,
    }
}

fn with_bridge<F: FnOnce(&dyn BrowserBridge)>(f: F) {
    if let Some(b) = browser_bridge() {
        f(b);
    }
}

/// Reports the last-known mouse position. Returns 1 if valid.
pub fn jfn_input_last_mouse_pos(
    out_x: &mut i32,
    out_y: &mut i32,
    out_modifiers: &mut u32,
) -> c_int {
    let p = *LAST_POS.lock();
    *out_x = p.x;
    *out_y = p.y;
    *out_modifiers = p.modifiers;
    if p.valid { 1 } else { 0 }
}

pub fn jfn_input_dispatch_mouse_move(x: i32, y: i32, mods: u32, leave: c_int) {
    {
        let mut p = LAST_POS.lock();
        if leave != 0 {
            p.valid = false;
        } else {
            p.valid = true;
            p.x = x;
            p.y = y;
            p.modifiers = mods;
        }
    }
    with_bridge(|b| b.send_mouse_move(x, y, mods, leave != 0));
}

pub fn jfn_input_dispatch_mouse_button(
    button_code: u32,
    pressed: c_int,
    x: i32,
    y: i32,
    mods: u32,
) {
    let Some(btn) = cef_button(button_code) else {
        return;
    };
    with_bridge(|b| b.send_mouse_click(x, y, mods, btn, pressed == 0, 1));
}

pub fn jfn_input_dispatch_scroll(x: i32, y: i32, dx: i32, dy: i32, mods: u32) {
    with_bridge(|b| b.send_mouse_wheel(x, y, mods, dx, dy));
}

/// Variant that lets the caller flag a precision (trackpad) delta.
pub fn jfn_input_dispatch_scroll_precise(
    x: i32,
    y: i32,
    dx: i32,
    dy: i32,
    mods: u32,
    precise: c_int,
) {
    let mods = if precise != 0 {
        mods | EVENTFLAG_PRECISION_SCROLLING_DELTA
    } else {
        mods
    };
    with_bridge(|b| b.send_mouse_wheel(x, y, mods, dx, dy));
}

pub fn jfn_input_dispatch_history_nav(forward: c_int) {
    with_bridge(|b| b.navigate_history(forward != 0));
}

pub fn jfn_input_dispatch_keyboard_focus(gained: c_int) {
    with_bridge(|b| b.set_focus(gained != 0));
}

/// Char event with explicit is_system_key (for WM_SYSCHAR on Windows). The
/// 3-arg `jfn_input_dispatch_char` below is the wayland/x11 path which never
/// generates system chars.
pub fn jfn_input_dispatch_char_sys(
    codepoint: u32,
    mods: u32,
    native_code: u32,
    is_system_key: c_int,
) {
    if codepoint == 0 || codepoint >= 0x10_FFFF {
        return;
    }
    let cp16 = codepoint as u16;
    with_bridge(|b| {
        b.send_key_event(
            KEYEVENT_CHAR,
            mods,
            codepoint as c_int,
            native_code as c_int,
            is_system_key != 0,
            cp16,
            cp16,
        );
    });
}

pub fn jfn_input_dispatch_char(codepoint: u32, mods: u32, native_code: u32) {
    jfn_input_dispatch_char_sys(codepoint, mods, native_code, 0);
}

/// Flat key dispatch used by macOS and Windows input shims. Linux paths use
/// [`jfn_input_dispatch_key_raw`] below, which routes through the xkb
/// keysym → VK mapping first.
pub fn jfn_input_dispatch_key_full(
    pressed: c_int,
    windows_key_code: i32,
    native_key_code: i32,
    modifiers: u32,
    character: u16,
    unmodified_character: u16,
    is_system_key: c_int,
) {
    if pressed != 0 {
        match jfn_hotkey_classify_keydown(windows_key_code, modifiers) {
            1 => {
                jfn_shutdown_initiate();
                return;
            }
            2 => {
                if let Some(p) = jfn_platform_abi::try_get() {
                    p.toggle_fullscreen();
                }
                return;
            }
            _ => {}
        }
    }
    let type_ = if pressed != 0 {
        KEYEVENT_RAWKEYDOWN
    } else {
        KEYEVENT_KEYUP
    };
    with_bridge(|b| {
        b.send_key_event(
            type_,
            modifiers,
            windows_key_code,
            native_key_code,
            is_system_key != 0,
            character,
            unmodified_character,
        );
    });
}

#[cfg(target_os = "linux")]
pub fn jfn_input_dispatch_key_raw(keysym: u32, native_code: u32, mods: u32, pressed: c_int) {
    // XKB_KEY_XF86Back / XKB_KEY_XF86Forward.
    const XF86_BACK: u32 = 0x1008FF26;
    const XF86_FORWARD: u32 = 0x1008FF27;
    if keysym == XF86_BACK || keysym == XF86_FORWARD {
        if pressed != 0 {
            jfn_input_dispatch_history_nav((keysym == XF86_FORWARD) as c_int);
        }
        return;
    }
    let vkey = keysym::keysym_to_vkey(keysym);
    // CEF on Linux expects an X11 keycode (evdev keycode + 8) for native_key_code.
    let native = native_code as i32 + 8;
    jfn_input_dispatch_key_full(pressed, vkey, native, mods, 0, 0, 0);
}
