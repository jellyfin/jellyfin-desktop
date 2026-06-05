//! Lifecycle glue for the X11 input thread.
//!
//! Holds the per-thread `Handle` in a static `Mutex<Option<...>>` so the
//! Platform-vtable cursor setter and the cleanup path can reach it from
//! any thread.

use crate::input::{Handle, set_cursor, start as start_thread};
use jfn_platform_abi::cursor::CursorShape;
use parking_lot::Mutex;

static G: Mutex<Option<Handle>> = Mutex::new(None);

pub fn start(parent: u32) {
    let m = crate::x11_state::MUT.lock();
    let screen_num = m.as_ref().map(|s| s.screen_num).unwrap_or(0);
    drop(m);
    if let Some(handle) = start_thread(screen_num, parent) {
        *G.lock() = Some(handle);
    }
}

pub fn cleanup() {
    let mut g = G.lock();
    if let Some(h) = g.as_mut() {
        h.join();
    }
    *g = None;
}

pub fn set_cursor_active(shape: CursorShape) {
    let g = G.lock();
    if let Some(h) = g.as_ref() {
        set_cursor(h, shape);
    }
}
