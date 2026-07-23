//! App-level CEF context-menu items appended to every browser's menu.
//!
//! The build/dispatch closures returned here are installed via
//! `JfnCefLayer::set_context_menu_builder_rust` /
//! `set_context_menu_dispatcher_rust` by each business wrapper.

use cef::rc::ConvertReturnValue;
use cef::{ImplMenuModel, MenuModel, sys};
use std::os::raw::{c_int, c_void};

use crate::i18n::{StringKey, text};

// Command IDs numbered from cef_menu_id_t::MENU_ID_USER_FIRST.
const MENU_ID_USER_FIRST: c_int = sys::cef_menu_id_t::MENU_ID_USER_FIRST as c_int;
pub const MENU_ID_TOGGLE_FULLSCREEN: c_int = MENU_ID_USER_FIRST;
pub const MENU_ID_ABOUT: c_int = MENU_ID_USER_FIRST + 1;
pub const MENU_ID_EXIT: c_int = MENU_ID_USER_FIRST + 2;

use jfn_playback::shutdown::jfn_shutdown_initiate;

/// Build closure for [`JfnCefLayer::set_context_menu_builder_rust`].
/// The slot invocation adds one ref to the menu model before calling this,
/// so we adopt it via `wrap_result` (no extra add_ref needed).
pub fn build_closure() -> Box<crate::client::ContextBuilderFn> {
    Box::new(|raw: *mut c_void| {
        if raw.is_null() {
            return;
        }
        let m: MenuModel = (raw as *mut sys::_cef_menu_model_t).wrap_result();
        m.add_item(
            MENU_ID_TOGGLE_FULLSCREEN,
            Some(&cef::CefString::from(text(StringKey::ToggleFullscreen))),
        );
        m.add_item(
            MENU_ID_ABOUT,
            Some(&cef::CefString::from(text(StringKey::About))),
        );
        m.add_item(
            MENU_ID_EXIT,
            Some(&cef::CefString::from(text(StringKey::Exit))),
        );
    })
}

/// Dispatch closure for [`JfnCefLayer::set_context_menu_dispatcher_rust`].
pub fn dispatch_closure() -> Box<crate::client::ContextDispatcherFn> {
    Box::new(|cmd: c_int| -> bool {
        if cmd == MENU_ID_TOGGLE_FULLSCREEN {
            if let Some(p) = jfn_platform_abi::try_get() {
                p.toggle_fullscreen();
            }
            true
        } else if cmd == MENU_ID_ABOUT {
            crate::business_about::jfn_about_open();
            true
        } else if cmd == MENU_ID_EXIT {
            jfn_shutdown_initiate();
            true
        } else {
            false
        }
    })
}
