//! Windows [`MpvHost`]: pre-create environment only.

use std::ffi::c_char;

use jfn_platform_abi::{MpvHost, WindowDecorations};

pub struct WindowsMpvHost;

impl MpvHost for WindowsMpvHost {
    fn prepare(&self, _decorations: WindowDecorations) {
        // Tell mpv to load the window icon from our exe resources. Set via
        // _putenv_s — mpv reads it through the CRT's getenv, which
        // SetEnvironmentVariableW (std::env::set_var) does not update.
        unsafe extern "C" {
            fn _putenv_s(name: *const c_char, value: *const c_char) -> i32;
        }
        let key = c"MPV_WINDOW_ICON";
        let val = c"IDI_ICON1";
        unsafe {
            _putenv_s(key.as_ptr(), val.as_ptr());
        }
    }
}
