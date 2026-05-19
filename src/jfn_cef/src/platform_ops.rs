//! C-ABI mirror of `JfnPlatformOps` (src/platform/platform_ops.h). The C++
//! side populates a process-static vtable of thunks that wrap `g_platform`
//! and hands it in once via [`jfn_cef_set_platform_ops`]. Subsequent slices
//! (resize, render handlers, fade, popup) dispatch through this table.
//!
//! Slice 2 wired the registration; slice 3+ dispatch through [`ops`].

#![allow(dead_code)]

use std::os::raw::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicPtr, Ordering};

#[repr(C)]
pub struct JfnRect {
    pub x: c_int,
    pub y: c_int,
    pub w: c_int,
    pub h: c_int,
}

#[repr(C)]
pub struct JfnPopupRequest {
    pub x: c_int,
    pub y: c_int,
    pub lw: c_int,
    pub lh: c_int,
    pub options: *const *const c_char,
    pub options_len: usize,
    pub initial_highlight: c_int,
    pub on_selected: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
    pub on_selected_ctx: *mut c_void,
    pub on_selected_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
}

#[repr(C)]
pub struct JfnPlatformOps {
    pub surface_present:
        Option<unsafe extern "C" fn(*mut c_void, *const c_void) -> bool>,
    pub surface_present_software: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const JfnRect,
            usize,
            *const c_void,
            c_int,
            c_int,
        ) -> bool,
    >,
    pub surface_resize:
        Option<unsafe extern "C" fn(*mut c_void, c_int, c_int, c_int, c_int)>,
    pub surface_set_visible: Option<unsafe extern "C" fn(*mut c_void, bool)>,

    pub fade_surface: Option<
        unsafe extern "C" fn(
            *mut c_void,
            f32,
            Option<unsafe extern "C" fn(*mut c_void)>,
            *mut c_void,
            Option<unsafe extern "C" fn(*mut c_void)>,
            Option<unsafe extern "C" fn(*mut c_void)>,
            *mut c_void,
            Option<unsafe extern "C" fn(*mut c_void)>,
        ),
    >,

    pub popup_show:
        Option<unsafe extern "C" fn(*mut c_void, *const JfnPopupRequest)>,
    pub popup_hide: Option<unsafe extern "C" fn(*mut c_void)>,
    pub popup_present:
        Option<unsafe extern "C" fn(*mut c_void, *const c_void, c_int, c_int)>,
    pub popup_present_software: Option<
        unsafe extern "C" fn(*mut c_void, *const c_void, c_int, c_int, c_int, c_int),
    >,

    pub set_fullscreen: Option<unsafe extern "C" fn(bool)>,
    pub set_cursor: Option<unsafe extern "C" fn(c_int)>,
    pub clipboard_read_text_async: Option<
        unsafe extern "C" fn(
            Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
            *mut c_void,
            Option<unsafe extern "C" fn(*mut c_void)>,
        ),
    >,
    pub open_external_url: Option<unsafe extern "C" fn(*const c_char, usize)>,
}

static OPS: AtomicPtr<JfnPlatformOps> = AtomicPtr::new(std::ptr::null_mut());

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_set_platform_ops(ops: *const JfnPlatformOps) {
    OPS.store(ops as *mut JfnPlatformOps, Ordering::Release);
}

pub fn ops() -> Option<&'static JfnPlatformOps> {
    let p = OPS.load(Ordering::Acquire);
    if p.is_null() {
        None
    } else {
        Some(unsafe { &*p })
    }
}
