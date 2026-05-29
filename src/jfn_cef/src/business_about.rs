//! AboutBrowser business logic.
//!
//! Self-managing singleton: `jfn_about_open` creates the layer via the
//! Browsers registry and installs handler closures via the JfnCefLayer
//! setters. The unified BeforeClose path in `client::handle_on_before_close`
//! auto-removes the layer from the registry; the Browsers active-stack
//! restores focus to the previous top automatically. This module just
//! tracks open/closed status via `OPEN`.

use cef::{ImplBrowser, ImplBrowserHost};
use std::ffi::CString;
use std::os::raw::c_void;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::browsers::{jfn_browsers_create, jfn_browsers_set_active};
use crate::business_common::{adopt_message_refs, list_string};
use crate::client::{
    jfn_cef_layer_create, jfn_cef_layer_inner, jfn_cef_layer_set_name, jfn_cef_layer_set_visible,
};
use crate::platform_ops;

static OPEN: AtomicBool = AtomicBool::new(false);

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

    let l = unsafe { &*layer };
    let inner = unsafe { jfn_cef_layer_inner(layer) };

    // setCreatedCallback — about wins input whenever it's created.
    let inner_for_created = Arc::clone(&inner);
    l.set_created_callback_rust(Some(Box::new(move |_browser_raw: *mut c_void| {
        let p = inner_for_created.layer_ptr();
        if !p.is_null() {
            jfn_browsers_set_active(p);
        }
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

    unsafe {
        jfn_cef_layer_set_visible(layer, true);
        let url = "app://resources/about.html";
        jfn_cef_layer_create(layer, url.as_ptr() as *const _, url.len());
    }
}

fn handle_message(name: &str, args_raw: *mut c_void, browser_raw: *mut c_void) -> bool {
    let (args, browser) = adopt_message_refs(args_raw, browser_raw);

    match name {
        "aboutDismiss" => {
            if let Some(b) = browser
                && let Some(host) = b.host()
            {
                host.close_browser(0);
            }
            true
        }
        "aboutOpenPath" => {
            let Some(args) = args else { return true };
            let path = list_string(&args, 0);
            if path.is_empty() {
                return true;
            }
            if let Some(p) = platform_ops::ops() {
                p.open_external_url(&format!("file://{}", path));
            }
            true
        }
        _ => false,
    }
}
