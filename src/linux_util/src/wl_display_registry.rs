//! Connects append in order with a monotonic index, so a restarted VO appends
//! rather than overwrites; the proxy's accept-order index
//! (`jfn_wlproxy_vo_connection_index`) selects the current VO's display.

use std::ffi::c_void;

use parking_lot::Mutex;

// `usize` rather than `*mut wl_display` so the registry is `Send`/`Sync`.
static CAPTURED: Mutex<Vec<usize>> = Mutex::new(Vec::new());

pub fn record_connect(display: *mut c_void) -> usize {
    let mut v = CAPTURED.lock();
    v.push(display as usize);
    v.len() - 1
}

pub fn captured_display(index: usize) -> *mut c_void {
    CAPTURED
        .lock()
        .get(index)
        .map(|&a| a as *mut c_void)
        .unwrap_or(std::ptr::null_mut())
}

pub fn connect_count() -> usize {
    CAPTURED.lock().len()
}
