//! Lifecycle glue for the X11 input thread.
//!
//! Holds the per-thread `Handle` in a static `Mutex<Option<...>>` so the
//! Platform-vtable cursor setter and the cleanup path can reach it from
//! any thread.

use std::sync::{Arc};
use parking_lot::Mutex;

use xcb::x;

use crate::input::{Handle, set_cursor, start as start_thread};

static G: Mutex<Option<Handle>> = Mutex::new(None);

pub fn start(conn: Arc<xcb::Connection>, parent: x::Window) {
    let m = crate::x11_state::MUT.lock();
    let screen_num = m.as_ref().map(|s| s.screen_num).unwrap_or(0);
    drop(m);
    let handle = start_thread(conn, screen_num, parent);
    *G.lock() = Some(handle);
}

pub fn cleanup() {
    let mut g = G.lock();
    if let Some(h) = g.as_mut() {
        h.join();
    }
    *g = None;
}

pub fn set_cursor_active(cef_cursor_type: u32) {
    let g = G.lock();
    if let Some(h) = g.as_ref() {
        set_cursor(h, cef_cursor_type);
    }
}
