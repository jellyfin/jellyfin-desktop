//! BrowserBridge impl forwarded by jfn_platform_abi.
//!
//! Lets the `input` and `macos` crates dispatch into whichever CEF layer
//! is currently active without holding any `*mut JfnCefLayer` themselves
//! (or depending on jfn-cef, which would cycle through `jfn-input`).

use std::os::raw::c_int;

use jfn_platform_abi::BrowserBridge;

use crate::browsers::jfn_browsers_active;
use crate::client::{
    jfn_cef_layer_can_go_back, jfn_cef_layer_can_go_forward, jfn_cef_layer_copy, jfn_cef_layer_cut,
    jfn_cef_layer_go_back, jfn_cef_layer_go_forward, jfn_cef_layer_paste, jfn_cef_layer_redo,
    jfn_cef_layer_select_all, jfn_cef_layer_send_key_event, jfn_cef_layer_send_mouse_click,
    jfn_cef_layer_send_mouse_move, jfn_cef_layer_send_mouse_wheel, jfn_cef_layer_set_focus,
    jfn_cef_layer_undo, jfn_cef_layer_view_scale,
};

pub struct CefBrowserBridge;

impl BrowserBridge for CefBrowserBridge {
    fn send_key_event(
        &self,
        type_: c_int,
        modifiers: u32,
        windows_key_code: c_int,
        native_key_code: c_int,
        is_system_key: bool,
        character: u16,
        unmodified_character: u16,
    ) {
        let l = jfn_browsers_active();
        if l.is_null() {
            return;
        }
        unsafe {
            jfn_cef_layer_send_key_event(
                l,
                type_,
                modifiers,
                windows_key_code,
                native_key_code,
                is_system_key,
                character,
                unmodified_character,
            );
        }
    }

    fn send_mouse_click(
        &self,
        x: c_int,
        y: c_int,
        modifiers: u32,
        button: c_int,
        mouse_up: bool,
        click_count: c_int,
    ) {
        let l = jfn_browsers_active();
        if l.is_null() {
            return;
        }
        unsafe {
            jfn_cef_layer_send_mouse_click(l, x, y, modifiers, button, mouse_up, click_count);
        }
    }

    fn send_mouse_move(&self, x: i32, y: i32, modifiers: u32, leave: bool) {
        let l = jfn_browsers_active();
        if l.is_null() {
            return;
        }
        unsafe { jfn_cef_layer_send_mouse_move(l, x, y, modifiers, leave) };
    }

    fn send_mouse_wheel(&self, x: c_int, y: c_int, modifiers: u32, delta_x: c_int, delta_y: c_int) {
        let l = jfn_browsers_active();
        if l.is_null() {
            return;
        }
        unsafe { jfn_cef_layer_send_mouse_wheel(l, x, y, modifiers, delta_x, delta_y) };
    }

    fn set_focus(&self, focus: bool) {
        let l = jfn_browsers_active();
        if !l.is_null() {
            unsafe { jfn_cef_layer_set_focus(l, focus) };
        }
    }

    fn navigate_history(&self, forward: bool) {
        let l = jfn_browsers_active();
        if l.is_null() {
            return;
        }
        unsafe {
            if forward {
                if jfn_cef_layer_can_go_forward(l) {
                    jfn_cef_layer_go_forward(l);
                }
            } else if jfn_cef_layer_can_go_back(l) {
                jfn_cef_layer_go_back(l);
            }
        }
    }

    fn undo(&self) {
        let l = jfn_browsers_active();
        if !l.is_null() {
            unsafe { jfn_cef_layer_undo(l) };
        }
    }
    fn redo(&self) {
        let l = jfn_browsers_active();
        if !l.is_null() {
            unsafe { jfn_cef_layer_redo(l) };
        }
    }
    fn cut(&self) {
        let l = jfn_browsers_active();
        if !l.is_null() {
            unsafe { jfn_cef_layer_cut(l) };
        }
    }
    fn copy(&self) {
        let l = jfn_browsers_active();
        if !l.is_null() {
            unsafe { jfn_cef_layer_copy(l) };
        }
    }
    fn paste(&self) {
        let l = jfn_browsers_active();
        if !l.is_null() {
            unsafe { jfn_cef_layer_paste(l) };
        }
    }
    fn select_all(&self) {
        let l = jfn_browsers_active();
        if !l.is_null() {
            unsafe { jfn_cef_layer_select_all(l) };
        }
    }

    fn has_active(&self) -> bool {
        !jfn_browsers_active().is_null()
    }

    fn view_scale(&self) -> f64 {
        let l = jfn_browsers_active();
        if l.is_null() {
            return 0.0;
        }
        unsafe { jfn_cef_layer_view_scale(l) }
    }
}

pub fn install() {
    jfn_platform_abi::install_browser_bridge(Box::new(CefBrowserBridge));
}
