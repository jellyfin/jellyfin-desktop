use std::os::raw::c_int;

use crate::keysym;
use jfn_input::{jfn_input_dispatch_history_nav, jfn_input_dispatch_key_full};

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
