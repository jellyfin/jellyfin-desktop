//! CefLayer state (Rust side).
//!
//! Slice 1: opaque handle holding the small bits of CefLayer state that have
//! no CEF dependency — name string, closed/loaded flags + condvars, debug
//! identity. C++ `CefLayer` (src/cef/cef_client.{h,cpp}) holds a
//! `JfnCefLayer*` and delegates state queries; CEF handler implementations
//! still live on the C++ side.
//!
//! Subsequent slices migrate resize/invalidate state, render handlers,
//! lifecycle handlers, callback slots, etc. into this module.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Condvar, Mutex};

pub struct JfnCefLayer {
    name: Mutex<String>,
    closed: AtomicBool,
    loaded: AtomicBool,
    close_mtx: Mutex<()>,
    close_cv: Condvar,
    load_mtx: Mutex<()>,
    load_cv: Condvar,
}

impl JfnCefLayer {
    fn new() -> Self {
        Self {
            name: Mutex::new(String::new()),
            closed: AtomicBool::new(false),
            loaded: AtomicBool::new(false),
            close_mtx: Mutex::new(()),
            close_cv: Condvar::new(),
            load_mtx: Mutex::new(()),
            load_cv: Condvar::new(),
        }
    }
}

unsafe fn handle<'a>(h: *const JfnCefLayer) -> &'a JfnCefLayer {
    unsafe { &*h }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_cef_layer_new() -> *mut JfnCefLayer {
    Box::into_raw(Box::new(JfnCefLayer::new()))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_free(h: *mut JfnCefLayer) {
    if h.is_null() {
        return;
    }
    drop(unsafe { Box::from_raw(h) });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_set_name(h: *const JfnCefLayer, s: *const c_char) {
    let layer = unsafe { handle(h) };
    let new = if s.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(s) }.to_string_lossy().into_owned()
    };
    *layer.name.lock().unwrap() = new;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_is_closed(h: *const JfnCefLayer) -> bool {
    unsafe { handle(h) }.closed.load(Ordering::Acquire)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_is_loaded(h: *const JfnCefLayer) -> bool {
    unsafe { handle(h) }.loaded.load(Ordering::Acquire)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_set_closed(h: *const JfnCefLayer, v: bool) {
    let l = unsafe { handle(h) };
    let _g = l.close_mtx.lock().unwrap();
    l.closed.store(v, Ordering::Release);
    l.close_cv.notify_all();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_set_loaded(h: *const JfnCefLayer, v: bool) {
    let l = unsafe { handle(h) };
    let _g = l.load_mtx.lock().unwrap();
    l.loaded.store(v, Ordering::Release);
    l.load_cv.notify_all();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_wait_for_close(h: *const JfnCefLayer) {
    let l = unsafe { handle(h) };
    let mut g = l.close_mtx.lock().unwrap();
    while !l.closed.load(Ordering::Acquire) {
        g = l.close_cv.wait(g).unwrap();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_wait_for_load(h: *const JfnCefLayer) {
    let l = unsafe { handle(h) };
    let mut g = l.load_mtx.lock().unwrap();
    while !l.loaded.load(Ordering::Acquire) {
        g = l.load_cv.wait(g).unwrap();
    }
}
