use cef::*;
use std::os::raw::c_int;
use std::sync::Arc;

use crate::client::Inner;
use crate::client_impl::os_ffi::OsKeyEvent;
use jfn_platform_abi::event_flags::{EVENTFLAG_ALT_DOWN, EVENTFLAG_CONTROL_DOWN};

fn action_modifier() -> u32 {
    jfn_platform_abi::try_get()
        .map(|p| p.display().action_modifier_flag())
        .unwrap_or(EVENTFLAG_CONTROL_DOWN)
}

fn is_paste_shortcut(e: &KeyEvent) -> bool {
    let kt: sys::cef_key_event_type_t = e.type_.into();
    if kt != sys::cef_key_event_type_t::KEYEVENT_RAWKEYDOWN {
        return false;
    }
    if (e.modifiers & action_modifier()) == 0 {
        return false;
    }
    if (e.modifiers & EVENTFLAG_ALT_DOWN) != 0 {
        return false;
    }
    e.windows_key_code == b'V' as i32
}

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
