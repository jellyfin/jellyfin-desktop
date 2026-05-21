//! Lifecycle wrapper around the Rust Wayland input thread.
//!
//! Owns the static `JfnInputWayland` handle, builds the input thread's
//! `Callbacks` struct from `extern "C"` dispatch shims defined in
//! `src/input/dispatch.cpp`, and exposes the Platform-vtable cursor setter.
//!
//! Replaces the former `src/input/input_wayland.cpp` glue file.

use std::ffi::{c_int, c_void};
use std::sync::atomic::{AtomicPtr, Ordering};

use crate::input::{Callbacks, JfnInputWayland};

unsafe extern "C" {
    fn jfn_input_dispatch_mouse_move(x: i32, y: i32, mods: u32, leave: c_int);
    fn jfn_input_dispatch_mouse_button(
        button: u32, pressed: c_int, x: i32, y: i32, mods: u32,
    );
    fn jfn_input_dispatch_scroll(x: i32, y: i32, dx: i32, dy: i32, mods: u32);
    fn jfn_input_dispatch_history_nav(forward: c_int);
    fn jfn_input_dispatch_keyboard_focus(gained: c_int);
    fn jfn_input_dispatch_key_raw(keysym: u32, native_code: u32, mods: u32, pressed: c_int);
    fn jfn_input_dispatch_char(codepoint: u32, mods: u32, native_code: u32);
}

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
