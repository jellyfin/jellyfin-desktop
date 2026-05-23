//! Shared C-ABI shape of the `Platform` vtable. Mirrors `struct Platform`
//! from `src/platform/platform.h` byte-for-byte (`#[repr(C)]`). Per-backend
//! crates (jfn-wayland, jfn-x11, jfn-macos, jfn-windows) populate this and
//! return it by value through their `make_*_platform` C entry point.
//!
//! Layout invariants are pinned by `static_assert`s in
//! `src/platform/platform_ops.cpp` — any drift surfaces at compile time on
//! the C++ side, before bad offsets reach a function call.

#![allow(non_snake_case)]

use std::ffi::{c_char, c_int, c_void};

#[repr(i32)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum DisplayBackend {
    Wayland = 0,
    X11 = 1,
    Windows = 2,
    MacOS = 3,
}

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
pub struct Platform {
    pub display: DisplayBackend,
    // 4 bytes implicit padding to align the first fn-ptr.
    pub early_init: Option<unsafe extern "C" fn()>,
    pub init: Option<unsafe extern "C" fn(*mut c_void) -> bool>,
    pub cleanup: Option<unsafe extern "C" fn()>,
    pub post_window_cleanup: Option<unsafe extern "C" fn()>,
    pub alloc_surface: Option<unsafe extern "C" fn() -> *mut c_void>,
    pub free_surface: Option<unsafe extern "C" fn(*mut c_void)>,
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
    pub surface_set_visible:
        Option<unsafe extern "C" fn(*mut c_void, bool)>,
    pub restack:
        Option<unsafe extern "C" fn(*const *mut c_void, usize)>,
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
        unsafe extern "C" fn(
            *mut c_void,
            *const c_void,
            c_int,
            c_int,
            c_int,
            c_int,
        ),
    >,
    pub set_fullscreen: Option<unsafe extern "C" fn(bool)>,
    pub toggle_fullscreen: Option<unsafe extern "C" fn()>,
    pub begin_transition: Option<unsafe extern "C" fn()>,
    pub end_transition: Option<unsafe extern "C" fn()>,
    pub in_transition: Option<unsafe extern "C" fn() -> bool>,
    pub set_expected_size: Option<unsafe extern "C" fn(c_int, c_int)>,
    pub get_scale: Option<unsafe extern "C" fn() -> f32>,
    pub get_display_scale: Option<unsafe extern "C" fn(c_int, c_int) -> f32>,
    pub query_window_position:
        Option<unsafe extern "C" fn(*mut c_int, *mut c_int) -> bool>,
    pub clamp_window_geometry: Option<
        unsafe extern "C" fn(*mut c_int, *mut c_int, *mut c_int, *mut c_int),
    >,
    pub pump: Option<unsafe extern "C" fn()>,
    pub run_main_loop: Option<unsafe extern "C" fn()>,
    pub wake_main_loop: Option<unsafe extern "C" fn()>,
    pub set_cursor: Option<unsafe extern "C" fn(c_int)>,
    pub set_idle_inhibit: Option<unsafe extern "C" fn(c_int)>,
    pub set_theme_color: Option<unsafe extern "C" fn(u32)>,
    pub shared_texture_supported: bool,
    // 7 bytes alignment padding before cef_ozone_platform.
    pub cef_ozone_platform: [c_char; 32],
    // 7 bytes alignment padding before the next fn-ptr.
    pub clipboard_read_text_async: Option<
        unsafe extern "C" fn(
            Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
            *mut c_void,
            Option<unsafe extern "C" fn(*mut c_void)>,
        ),
    >,
    pub open_external_url: Option<unsafe extern "C" fn(*const c_char, usize)>,
}
