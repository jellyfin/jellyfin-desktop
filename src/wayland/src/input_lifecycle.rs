//! Lifecycle wrapper around the Rust Wayland input thread.
//!
//! Owns the static `JfnInputWayland` handle, builds the input thread's
//! `Callbacks` struct from `extern "C"` dispatch shims defined in
//! `src/input/dispatch.cpp`, and exposes the Platform-vtable cursor setter.
//!
//! Replaces the former `src/input/input_wayland.cpp` glue file.

use std::ffi::c_void;
use std::sync::atomic::{AtomicPtr, Ordering};

use crate::input::{Callbacks, JfnInputWayland};

use jfn_input::{
    jfn_input_dispatch_char, jfn_input_dispatch_history_nav, jfn_input_dispatch_key_raw,
    jfn_input_dispatch_keyboard_focus, jfn_input_dispatch_mouse_button,
    jfn_input_dispatch_mouse_move, jfn_input_dispatch_scroll,
};

const CALLBACKS: Callbacks = Callbacks {
    mouse_move:   Some(jfn_input_dispatch_mouse_move),
    mouse_button: Some(jfn_input_dispatch_mouse_button),
    scroll:       Some(jfn_input_dispatch_scroll),
    history_nav:  Some(jfn_input_dispatch_history_nav),
    kb_focus:     Some(jfn_input_dispatch_keyboard_focus),
    key:          Some(jfn_input_dispatch_key_raw),
    char_:        Some(jfn_input_dispatch_char),
};

static G_CTX: AtomicPtr<JfnInputWayland> = AtomicPtr::new(std::ptr::null_mut());

pub fn lifecycle_init(display: *mut c_void) {
    let ptr = unsafe { crate::input::jfn_input_wayland_init(display, &CALLBACKS) };
    G_CTX.store(ptr, Ordering::Release);
}

pub fn lifecycle_start() {
    let ptr = G_CTX.load(Ordering::Acquire);
    if !ptr.is_null() {
        unsafe { crate::input::jfn_input_wayland_start(ptr) };
    }
}

pub fn lifecycle_cleanup() {
    let ptr = G_CTX.swap(std::ptr::null_mut(), Ordering::AcqRel);
    if !ptr.is_null() {
        unsafe { crate::input::jfn_input_wayland_cleanup(ptr) };
    }
}

pub fn set_cursor_active(cef_cursor_type: u32) {
    let ptr = G_CTX.load(Ordering::Acquire);
    if !ptr.is_null() {
        unsafe { crate::input::jfn_input_wayland_set_cursor(ptr, cef_cursor_type) };
    }
}
