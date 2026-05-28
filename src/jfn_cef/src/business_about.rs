//! AboutBrowser business logic.
//!
//! Self-managing singleton: `jfn_about_open` creates the layer via the
//! Browsers registry and installs handler closures via the JfnCefLayer
//! setters. The unified BeforeClose path in `client::handle_on_before_close`
//! auto-removes the layer from the registry; the Browsers active-stack
//! restores focus to the previous top automatically. This module just
//! tracks open/closed status via INSTANCE.

use cef::rc::ConvertReturnValue;
use cef::{Browser, CefString, ImplBrowser, ImplBrowserHost, ImplListValue, ListValue, sys};
use std::ffi::CString;
use std::os::raw::c_void;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::client::JfnCefLayer;
use crate::platform_ops;

use crate::browsers::{jfn_browsers_create, jfn_browsers_set_active};
use crate::client::{jfn_cef_layer_create, jfn_cef_layer_set_name, jfn_cef_layer_set_visible};

static OPEN: AtomicBool = AtomicBool::new(false);

pub fn jfn_about_is_open() -> bool {
    OPEN.load(Ordering::Acquire)
}

/// Entry point. Creates the about layer and installs all Rust handler
/// closures. Subsequent calls while the layer is alive are no-ops.
pub fn jfn_about_open() {
    if OPEN
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        return;
    }

    let kind = CString::new("about").unwrap();
    let layer = unsafe { jfn_browsers_create(kind.as_ptr()) };
    if layer.is_null() {
        OPEN.store(false, Ordering::Release);
        return;
    }

    let name = CString::new("about").unwrap();
    unsafe { jfn_cef_layer_set_name(layer, name.as_ptr()) };

    install_handlers(layer);

    unsafe {
        jfn_cef_layer_set_visible(layer, true);
        let url = "app://resources/about.html";
        jfn_cef_layer_create(layer, url.as_ptr() as *const _, url.len());
    }
}

fn install_handlers(layer: *mut JfnCefLayer) {
    let l = unsafe { &*layer };

    // setCreatedCallback — about wins input whenever it's created.
    let layer_for_created = LayerPtr(layer);
    l.set_created_callback_rust(Some(Box::new(move |_browser_raw: *mut c_void| {
        let lp = &layer_for_created;
        jfn_browsers_set_active(lp.0);
    })));

    // setMessageHandler — aboutDismiss / aboutOpenPath.
    l.set_message_handler_rust(Some(Box::new(
        move |name: &str, args_raw: *mut c_void, browser_raw: *mut c_void| -> bool {
            handle_message(name, args_raw, browser_raw)
        },
    )));

    // setContextMenuBuilder / dispatcher — shared app menu.
    l.set_context_menu_builder_rust(Some(crate::app_menu::build_closure()));
    l.set_context_menu_dispatcher_rust(Some(crate::app_menu::dispatch_closure()));

    // BeforeClose: clear the open-status singleton. The Browsers registry
    // removal + active-stack pop are handled unconditionally by
    // `client::handle_on_before_close`.
    l.set_before_close_callback_rust(Some(Box::new(|| {
        OPEN.store(false, Ordering::Release);
    })));
}

fn handle_message(name: &str, args_raw: *mut c_void, browser_raw: *mut c_void) -> bool {
    if name == "aboutDismiss" {
        if !browser_raw.is_null() {
            let browser: Browser = (browser_raw as *mut sys::_cef_browser_t).wrap_result();
            if let Some(host) = browser.host() {
                host.close_browser(0);
            }
        }
        if !args_raw.is_null() {
            let _: ListValue = (args_raw as *mut sys::_cef_list_value_t).wrap_result();
        }
        return true;
    }
    if name == "aboutOpenPath" {
        let mut path = String::new();
        if !args_raw.is_null() {
            let args: ListValue = (args_raw as *mut sys::_cef_list_value_t).wrap_result();
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
    if !args_raw.is_null() {
        let _: ListValue = (args_raw as *mut sys::_cef_list_value_t).wrap_result();
    }
    if !browser_raw.is_null() {
        let _: Browser = (browser_raw as *mut sys::_cef_browser_t).wrap_result();
    }
    false
}

#[derive(Clone, Copy)]
struct LayerPtr(*mut JfnCefLayer);
unsafe impl Send for LayerPtr {}
unsafe impl Sync for LayerPtr {}
