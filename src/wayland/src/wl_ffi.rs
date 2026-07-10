//! Thunks marshalling raw inputs into [`crate::wl_ops`] entry points.
//!
//! Opaque surface handles travel as `*mut c_void` aliasing a
//! `PlatformSurface*` allocated by [`jfn_wl_alloc_surface`].
//!
//! # Safety
//!
//! Every `pub unsafe fn` here accepts raw handles preserved from the
//! original FFI surface. Callers must pass either null or a handle
//! returned by `jfn_wl_alloc_surface`, along with valid frame/pixels
//! buffers for the documented dimensions.

#![allow(clippy::missing_safety_doc)]

use std::ffi::c_void;
use std::slice;

use crate::wl_ops::{self, JfnDmabufFrame};
use crate::wl_state::PlatformSurface;

#[inline]
fn cast(handle: *mut c_void) -> *mut PlatformSurface {
    handle as *mut PlatformSurface
}

// =====================================================================
// Lifecycle
// =====================================================================

pub unsafe fn jfn_wl_core_init(display: *mut c_void) -> bool {
    match unsafe { crate::wl_state::init(display) } {
        Ok(()) => true,
        Err(e) => {
            tracing::error!("jfn_wl_core_init: {e}");
            false
        }
    }
}

pub fn jfn_wl_core_set_was_fullscreen(fs: bool) {
    if let Some(m) = crate::wl_state::try_state() {
        m.lock().was_fullscreen = fs;
    }
}

// =====================================================================
// Surface lifecycle
// =====================================================================

pub fn jfn_wl_alloc_surface() -> *mut c_void {
    wl_ops::alloc_surface() as *mut c_void
}

pub unsafe fn jfn_wl_free_surface(handle: *mut c_void) {
    wl_ops::free_surface(cast(handle));
}

pub unsafe fn jfn_wl_restack(handles: *const *mut c_void, n: usize) {
    if handles.is_null() || n == 0 {
        wl_ops::restack(&[]);
        return;
    }
    let slice = unsafe { slice::from_raw_parts(handles as *const *mut PlatformSurface, n) };
    wl_ops::restack(slice);
}

pub unsafe fn jfn_wl_surface_set_visible(
    handle: *mut c_void,
    visible: bool,
    bg_r: u8,
    bg_g: u8,
    bg_b: u8,
) {
    wl_ops::surface_set_visible(cast(handle), visible, bg_r, bg_g, bg_b);
}

// =====================================================================
// Paint
// =====================================================================

pub unsafe fn jfn_wl_surface_present(handle: *mut c_void, frame: *const JfnDmabufFrame) -> bool {
    if frame.is_null() {
        return false;
    }
    let frame = unsafe { &*frame };
    wl_ops::surface_present(cast(handle), frame)
}

pub unsafe fn jfn_wl_surface_present_software(
    handle: *mut c_void,
    pixels: *const u8,
    w: i32,
    h: i32,
) -> bool {
    if pixels.is_null() || w <= 0 || h <= 0 {
        return false;
    }
    let len = (w as usize)
        .checked_mul(h as usize)
        .and_then(|n| n.checked_mul(4));
    let Some(len) = len else { return false };
    let pixels = unsafe { slice::from_raw_parts(pixels, len) };
    wl_ops::surface_present_software(cast(handle), &[], pixels, w, h)
}

// =====================================================================
// Fullscreen
// =====================================================================

pub fn jfn_wl_was_fullscreen() -> bool {
    wl_ops::was_fullscreen()
}

pub fn jfn_wl_set_fullscreen(fullscreen: bool) {
    crate::root_window::set_fullscreen(fullscreen);
}

pub fn jfn_wl_toggle_fullscreen() {
    crate::root_window::toggle_fullscreen();
}

// =====================================================================
// Window controls (client-side decorations)
// =====================================================================

static MAXIMIZED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

pub fn jfn_wl_window_minimize() {
    crate::root_window::set_minimized();
}

pub fn jfn_wl_window_toggle_maximize() {
    use std::sync::atomic::Ordering;
    let next = !MAXIMIZED.load(Ordering::Relaxed);
    MAXIMIZED.store(next, Ordering::Relaxed);
    crate::root_window::set_maximized(next);
}

/// Mirror the compositor's maximized state into the toggle's command atomic;
/// without it a compositor-initiated maximize desyncs the toggle button.
pub fn sync_maximized_command_state(maximized: bool) {
    use std::sync::atomic::Ordering;
    MAXIMIZED.store(maximized, Ordering::Relaxed);
}

pub fn jfn_wl_window_start_move() {
    crate::root_window::start_move();
}

pub fn jfn_wl_window_start_resize(edge: i32) {
    crate::root_window::start_resize(edge as u32);
}
