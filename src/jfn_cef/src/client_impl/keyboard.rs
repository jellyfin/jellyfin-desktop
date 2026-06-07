use cef::*;
use std::os::raw::c_int;
use std::sync::Arc;

use crate::client::Inner;

#[cfg(target_os = "linux")]
type OsKeyEvent<'a> = Option<&'a mut sys::XEvent>;
#[cfg(target_os = "macos")]
type OsKeyEvent<'a> = *mut u8;
#[cfg(target_os = "windows")]
type OsKeyEvent<'a> = Option<&'a mut sys::MSG>;

// cef_event_flags_t.0 is i32 on non-macos, u32 on macos; cast keeps both green.
#[allow(clippy::unnecessary_cast)]
#[cfg(target_os = "macos")]
const ACTION_MODIFIER: u32 = sys::cef_event_flags_t::EVENTFLAG_COMMAND_DOWN.0 as u32;
#[allow(clippy::unnecessary_cast)]
#[cfg(not(target_os = "macos"))]
const ACTION_MODIFIER: u32 = sys::cef_event_flags_t::EVENTFLAG_CONTROL_DOWN.0 as u32;
#[allow(clippy::unnecessary_cast)]
const ALT_FLAG: u32 = sys::cef_event_flags_t::EVENTFLAG_ALT_DOWN.0 as u32;

fn is_paste_shortcut(e: &KeyEvent) -> bool {
    let kt: sys::cef_key_event_type_t = e.type_.into();
    if kt != sys::cef_key_event_type_t::KEYEVENT_RAWKEYDOWN {
        return false;
    }
    if (e.modifiers & ACTION_MODIFIER) == 0 {
        return false;
    }
    if (e.modifiers & ALT_FLAG) != 0 {
        return false;
    }
    e.windows_key_code == b'V' as i32
}

// ----- KeyboardHandler -----------------------------------------------------

wrap_keyboard_handler! {
    pub struct JfnKeyboardHandlerBuilder {
        inner: Arc<Inner>,
    }

    impl KeyboardHandler {
        fn on_pre_key_event(
            &self,
            _browser: Option<&mut Browser>,
            event: Option<&KeyEvent>,
            _os_event: OsKeyEvent<'_>,
            _is_keyboard_shortcut: Option<&mut c_int>,
        ) -> c_int {
            let Some(e) = event else { return 0 };
            if !is_paste_shortcut(e) {
                return 0;
            }
            if self.inner.try_paste() { 1 } else { 0 }
        }
    }
}
