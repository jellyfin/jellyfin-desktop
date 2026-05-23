//! Input dispatch. Ports `src/input/dispatch.cpp`.
//!
//! Platform translators (Rust: x11, wayland; C++: input_windows.cpp,
//! input_macos.mm via dispatch.h shims) call the `jfn_input_dispatch_*`
//! extern "C" entry points. We resolve the active CEF layer via
//! `jfn_browsers_active()` and forward via the layer C ABI.

use std::os::raw::c_int;
use std::sync::Mutex;

#[cfg(target_os = "linux")]
mod keysym;

// Opaque layer handle owned by jfn_cef; we only ever move pointers around.
#[repr(C)]
struct JfnCefLayer {
    _private: [u8; 0],
}

unsafe extern "C" {
    fn jfn_browsers_active() -> *const JfnCefLayer;

    fn jfn_cef_layer_can_go_back(h: *const JfnCefLayer) -> bool;
    fn jfn_cef_layer_can_go_forward(h: *const JfnCefLayer) -> bool;
    fn jfn_cef_layer_go_back(h: *const JfnCefLayer);
    fn jfn_cef_layer_go_forward(h: *const JfnCefLayer);
    fn jfn_cef_layer_set_focus(h: *const JfnCefLayer, focus: bool);
    fn jfn_cef_layer_send_key_event(
        h: *const JfnCefLayer,
        type_: c_int,
        modifiers: u32,
        windows_key_code: c_int,
        native_key_code: c_int,
        is_system_key: bool,
        character: u16,
        unmodified_character: u16,
    );
    fn jfn_cef_layer_send_mouse_click(
        h: *const JfnCefLayer,
        x: c_int,
        y: c_int,
        modifiers: u32,
        button: c_int,
        mouse_up: bool,
        click_count: c_int,
    );
    fn jfn_cef_layer_send_mouse_move(
        h: *const JfnCefLayer,
        x: i32,
        y: i32,
        modifiers: u32,
        leave: bool,
    );
    fn jfn_cef_layer_send_mouse_wheel(
        h: *const JfnCefLayer,
        x: c_int,
        y: c_int,
        modifiers: u32,
        delta_x: c_int,
        delta_y: c_int,
    );

    // Hotkey classifier lives in jfn-playback.
    fn jfn_hotkey_classify_keydown(windows_key_code: i32, modifiers: u32) -> u8;

    // Shutdown + fullscreen toggle bridges.
    fn jfn_shutdown_initiate();
    fn jfn_platform_toggle_fullscreen();
}

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

#[inline]
fn active_layer() -> *const JfnCefLayer {
    unsafe { jfn_browsers_active() }
}

fn cef_button(button_code: u32) -> Option<c_int> {
    match button_code {
        0x110 => Some(MBT_LEFT),
        0x111 => Some(MBT_RIGHT),
        0x112 => Some(MBT_MIDDLE),
        _ => None,
    }
}

// ---- extern "C" entry points ----

/// Reports the last-known mouse position. Returns 1 if valid.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_last_mouse_pos(
    out_x: *mut i32,
    out_y: *mut i32,
    out_modifiers: *mut u32,
) -> c_int {
    let p = *LAST_POS.lock().unwrap();
    unsafe {
        if !out_x.is_null() {
            *out_x = p.x;
        }
        if !out_y.is_null() {
            *out_y = p.y;
        }
        if !out_modifiers.is_null() {
            *out_modifiers = p.modifiers;
        }
    }
    if p.valid { 1 } else { 0 }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_dispatch_mouse_move(x: i32, y: i32, mods: u32, leave: c_int) {
    {
        let mut p = LAST_POS.lock().unwrap();
        if leave != 0 {
            p.valid = false;
        } else {
            p.valid = true;
            p.x = x;
            p.y = y;
            p.modifiers = mods;
        }
    }
    let l = active_layer();
    if l.is_null() {
        return;
    }
    unsafe { jfn_cef_layer_send_mouse_move(l, x, y, mods, leave != 0) };
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_dispatch_mouse_button(
    button_code: u32,
    pressed: c_int,
    x: i32,
    y: i32,
    mods: u32,
) {
    let Some(btn) = cef_button(button_code) else {
        return;
    };
    let l = active_layer();
    if l.is_null() {
        return;
    }
    unsafe { jfn_cef_layer_send_mouse_click(l, x, y, mods, btn, pressed == 0, 1) };
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_dispatch_scroll(x: i32, y: i32, dx: i32, dy: i32, mods: u32) {
    let l = active_layer();
    if l.is_null() {
        return;
    }
    unsafe { jfn_cef_layer_send_mouse_wheel(l, x, y, mods, dx, dy) };
}

/// Variant that lets the caller flag a precision (trackpad) delta.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_dispatch_scroll_precise(
    x: i32,
    y: i32,
    dx: i32,
    dy: i32,
    mods: u32,
    precise: c_int,
) {
    let l = active_layer();
    if l.is_null() {
        return;
    }
    let mods = if precise != 0 {
        mods | EVENTFLAG_PRECISION_SCROLLING_DELTA
    } else {
        mods
    };
    unsafe { jfn_cef_layer_send_mouse_wheel(l, x, y, mods, dx, dy) };
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_dispatch_history_nav(forward: c_int) {
    let l = active_layer();
    if l.is_null() {
        return;
    }
    unsafe {
        if forward != 0 {
            if jfn_cef_layer_can_go_forward(l) {
                jfn_cef_layer_go_forward(l);
            }
        } else if jfn_cef_layer_can_go_back(l) {
            jfn_cef_layer_go_back(l);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_dispatch_keyboard_focus(gained: c_int) {
    let l = active_layer();
    if !l.is_null() {
        unsafe { jfn_cef_layer_set_focus(l, gained != 0) };
    }
}

/// Char event with explicit is_system_key (for WM_SYSCHAR on Windows). The
/// 3-arg `jfn_input_dispatch_char` below is the wayland/x11 path which never
/// generates system chars.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_dispatch_char_sys(
    codepoint: u32,
    mods: u32,
    native_code: u32,
    is_system_key: c_int,
) {
    if codepoint == 0 || codepoint >= 0x10_FFFF {
        return;
    }
    let l = active_layer();
    if l.is_null() {
        return;
    }
    let cp16 = codepoint as u16;
    unsafe {
        jfn_cef_layer_send_key_event(
            l,
            KEYEVENT_CHAR,
            mods,
            codepoint as c_int,
            native_code as c_int,
            is_system_key != 0,
            cp16,
            cp16,
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_dispatch_char(codepoint: u32, mods: u32, native_code: u32) {
    jfn_input_dispatch_char_sys(codepoint, mods, native_code, 0);
}

/// Flat key dispatch used by C++ shim (input_macos.mm, input_windows.cpp). The
/// Linux paths use `jfn_input_dispatch_key_raw` below, which goes through the
/// xkb keysym → VK mapping first.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_dispatch_key_full(
    pressed: c_int,
    windows_key_code: i32,
    native_key_code: i32,
    modifiers: u32,
    character: u16,
    unmodified_character: u16,
    is_system_key: c_int,
) {
    if pressed != 0 {
        match unsafe { jfn_hotkey_classify_keydown(windows_key_code, modifiers) } {
            1 => {
                unsafe { jfn_shutdown_initiate() };
                return;
            }
            2 => {
                unsafe { jfn_platform_toggle_fullscreen() };
                return;
            }
            _ => {}
        }
    }
    let l = active_layer();
    if l.is_null() {
        return;
    }
    let type_ = if pressed != 0 {
        KEYEVENT_RAWKEYDOWN
    } else {
        KEYEVENT_KEYUP
    };
    unsafe {
        jfn_cef_layer_send_key_event(
            l,
            type_,
            modifiers,
            windows_key_code,
            native_key_code,
            is_system_key != 0,
            character,
            unmodified_character,
        );
    }
}

#[cfg(target_os = "linux")]
#[unsafe(no_mangle)]
pub extern "C" fn jfn_input_dispatch_key_raw(
    keysym: u32,
    native_code: u32,
    mods: u32,
    pressed: c_int,
) {
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
