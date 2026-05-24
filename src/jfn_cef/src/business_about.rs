//! AboutBrowser business logic. Ports `src/browser/about_browser.{cpp,h}`.
//!
//! Self-managing singleton: `jfn_about_open` creates the layer via the
//! C++ Browsers registry (FFI), installs handler closures via the Rust
//! JfnCefLayer setters, and stores layer + prev-active in INSTANCE. On
//! BeforeClose the singleton clears itself and removes the layer from
//! Browsers.

use cef::rc::ConvertReturnValue;
use cef::{Browser, CefString, ImplBrowser, ImplBrowserHost, ImplListValue, ListValue, sys};
use std::ffi::CString;
use std::os::raw::c_void;
use std::sync::Mutex;

use crate::client::JfnCefLayer;
use crate::platform_ops;

use crate::browsers::{
    jfn_browsers_active, jfn_browsers_create, jfn_browsers_remove, jfn_browsers_set_active,
};
use crate::client::{jfn_cef_layer_create, jfn_cef_layer_set_name, jfn_cef_layer_set_visible};

// Pointers in this struct refer to layers owned by the C++ Browsers vector.
// Safe to send across threads because the C++ owner is stable for the
// lifetime of the singleton (clear happens in BeforeClose).
struct AboutState {
    prev_active: *mut JfnCefLayer,
}

unsafe impl Send for AboutState {}

static INSTANCE: Mutex<Option<AboutState>> = Mutex::new(None);

pub fn jfn_about_is_open() -> bool {
    INSTANCE.lock().unwrap().is_some()
}

/// Entry point. Creates the about layer and installs all Rust handler
/// closures. Subsequent calls while the layer is alive are no-ops.
pub fn jfn_about_open() {
    {
        let g = INSTANCE.lock().unwrap();
        if g.is_some() {
            return;
        }
    }

    let kind = CString::new("about").unwrap();
    let layer = unsafe { jfn_browsers_create(kind.as_ptr()) };
    if layer.is_null() {
        return;
    }
    let prev_active = jfn_browsers_active();

    let name = CString::new("about").unwrap();
    unsafe { jfn_cef_layer_set_name(layer, name.as_ptr()) };

    install_handlers(layer, prev_active);

    unsafe {
        jfn_cef_layer_set_visible(layer, true);
        let url = "app://resources/about.html";
        jfn_cef_layer_create(layer, url.as_ptr() as *const _, url.len());
    }

    *INSTANCE.lock().unwrap() = Some(AboutState { prev_active });
}

fn install_handlers(layer: *mut JfnCefLayer, _prev_active: *mut JfnCefLayer) {
    let l = unsafe { &*layer };

    // setCreatedCallback — overlay wins input whenever it's created.
    let layer_for_created = LayerPtr(layer);
    l.set_created_callback_rust(Some(Box::new(move |_browser_raw: *mut c_void| {
        let lp = &layer_for_created;
        jfn_browsers_set_active(lp.0);
    })));

    // setMessageHandler — aboutDismiss / aboutOpenPath.
    let layer_for_msg = LayerPtr(layer);
    l.set_message_handler_rust(Some(Box::new(
        move |name: &str, args_raw: *mut c_void, browser_raw: *mut c_void| -> bool {
            let lp = &layer_for_msg;
            handle_message(name, args_raw, browser_raw, lp.0)
        },
    )));

    // setContextMenuBuilder / dispatcher — shared app menu.
    l.set_context_menu_builder_rust(Some(crate::app_menu::build_closure()));
    l.set_context_menu_dispatcher_rust(Some(crate::app_menu::dispatch_closure()));

    // setBeforeCloseCallback — clear singleton + tell Browsers to drop.
    let layer_for_close = LayerPtr(layer);
    l.set_before_close_callback_rust(Some(Box::new(move || {
        let lp = &layer_for_close;
        let prev = INSTANCE.lock().unwrap().take().map(|s| s.prev_active);
        if let Some(p) = prev {
            // Restore the previously active layer if the user dismissed via
            // close-without-aboutDismiss (e.g. ctx menu → Exit → re-open).
            jfn_browsers_set_active(p);
        }
        jfn_browsers_remove(lp.0);
    })));
}

fn handle_message(
    name: &str,
    args_raw: *mut c_void,
    browser_raw: *mut c_void,
    _layer: *mut JfnCefLayer,
) -> bool {
    if name == "aboutDismiss" {
        let prev = INSTANCE
            .lock()
            .unwrap()
            .as_ref()
            .map(|s| s.prev_active)
            .unwrap_or(std::ptr::null_mut());
        jfn_browsers_set_active(prev);
        if !browser_raw.is_null() {
            let browser: Browser =
                (browser_raw as *mut sys::_cef_browser_t).wrap_result();
            if let Some(host) = browser.host() {
                host.close_browser(0);
            }
        } else {
            // Drop the adopted list ref even if no browser; nothing else needs it.
        }
        // Adopt and drop the args ref so we don't leak it.
        if !args_raw.is_null() {
            let _: ListValue = (args_raw as *mut sys::_cef_list_value_t).wrap_result();
        }
        return true;
    }
    if name == "aboutOpenPath" {
        let mut path = String::new();
        if !args_raw.is_null() {
            let args: ListValue =
                (args_raw as *mut sys::_cef_list_value_t).wrap_result();
            let userfree = args.string(0);
            let cs: CefString = (&userfree).into();
            path = cs.to_string();
        }
        if !browser_raw.is_null() {
            let _: Browser = (browser_raw as *mut sys::_cef_browser_t).wrap_result();
        }
        if path.is_empty() {
            return true;
        }
        if let Some(p) = platform_ops::ops() {
            p.open_external_url(&format!("file://{}", path));
        }
        return true;
    }
    // Unhandled — still adopt-and-drop refs so we don't leak.
    if !args_raw.is_null() {
        let _: ListValue = (args_raw as *mut sys::_cef_list_value_t).wrap_result();
    }
    if !browser_raw.is_null() {
        let _: Browser = (browser_raw as *mut sys::_cef_browser_t).wrap_result();
    }
    false
}

// Newtype so closures capturing a *mut JfnCefLayer can be Send.
#[derive(Clone, Copy)]
struct LayerPtr(*mut JfnCefLayer);
unsafe impl Send for LayerPtr {}
unsafe impl Sync for LayerPtr {}
