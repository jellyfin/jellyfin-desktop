//! Rust author of the Windows `Platform` vtable.
//!
//! Composition only — individual platform functions still live in
//! `src/platform/windows.cpp` + `src/input/input_windows.cpp`. They are
//! exposed with `extern "C"` linkage so this crate can populate the
//! vtable from them by symbol name. Subsequent slices replace each
//! thunk with a native Rust implementation; the C ABI at the vtable
//! boundary stays stable.

#![cfg(target_os = "windows")]
#![allow(non_snake_case)]

use std::ffi::{c_char, c_int, c_void};

pub use jfn_platform_abi::{DisplayBackend, JfnPopupRequest, JfnRect, Platform};

unsafe extern "C" {
    fn win_early_init();
    fn win_init(mpv: *mut c_void) -> bool;
    fn win_cleanup();
    fn win_alloc_surface() -> *mut c_void;
    fn win_free_surface(s: *mut c_void);
    fn win_surface_present(s: *mut c_void, info: *const c_void) -> bool;
    fn win_surface_present_software(
        s: *mut c_void,
        dirty: *const JfnRect,
        dirty_len: usize,
        buffer: *const c_void,
        w: c_int,
        h: c_int,
    ) -> bool;
    fn win_surface_resize(s: *mut c_void, lw: c_int, lh: c_int, pw: c_int, ph: c_int);
    fn win_surface_set_visible(s: *mut c_void, visible: bool);
    fn win_restack(ordered: *const *mut c_void, n: usize);
    fn win_fade_surface(
        s: *mut c_void,
        fade_sec: f32,
        on_fade_start: Option<unsafe extern "C" fn(*mut c_void)>,
        start_ctx: *mut c_void,
        start_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
        on_complete: Option<unsafe extern "C" fn(*mut c_void)>,
        done_ctx: *mut c_void,
        done_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    );
    fn win_popup_show(s: *mut c_void, req: *const JfnPopupRequest);
    fn win_popup_hide(s: *mut c_void);
    fn win_popup_present(s: *mut c_void, info: *const c_void, lw: c_int, lh: c_int);
    fn win_popup_present_software(
        s: *mut c_void,
        buffer: *const c_void,
        pw: c_int,
        ph: c_int,
        lw: c_int,
        lh: c_int,
    );
    fn win_set_fullscreen(fullscreen: bool);
    fn win_toggle_fullscreen();
    fn win_begin_transition();
    fn win_end_transition();
    fn win_in_transition() -> bool;
    fn win_set_expected_size(w: c_int, h: c_int);
    fn win_get_scale() -> f32;
    fn win_get_display_scale(x: c_int, y: c_int) -> f32;
    fn win_query_window_position(x: *mut c_int, y: *mut c_int) -> bool;
    fn win_clamp_window_geometry(
        w: *mut c_int,
        h: *mut c_int,
        x: *mut c_int,
        y: *mut c_int,
    );
    fn win_pump();
    fn win_set_idle_inhibit(level: c_int);
    fn win_set_theme_color(rgb: u32);
    fn win_clipboard_read_text_async(
        on_done: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
        ctx: *mut c_void,
        dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    );
    fn win_open_external_url(utf8: *const c_char, len: usize);
    fn jfn_input_windows_set_cursor(t: c_int);
}

/// Returns the Windows `Platform` vtable by value across the C ABI.
/// Called from C++ `make_platform(DisplayBackend::Windows)` (platform.h).
#[unsafe(no_mangle)]
pub extern "C" fn make_windows_platform() -> Platform {
    Platform {
        display: DisplayBackend::Windows,
        early_init: Some(win_early_init),
        init: Some(win_init),
        cleanup: Some(win_cleanup),
        post_window_cleanup: None,
        alloc_surface: Some(win_alloc_surface),
        free_surface: Some(win_free_surface),
        surface_present: Some(win_surface_present),
        surface_present_software: Some(win_surface_present_software),
        surface_resize: Some(win_surface_resize),
        surface_set_visible: Some(win_surface_set_visible),
        restack: Some(win_restack),
        fade_surface: Some(win_fade_surface),
        popup_show: Some(win_popup_show),
        popup_hide: Some(win_popup_hide),
        popup_present: Some(win_popup_present),
        popup_present_software: Some(win_popup_present_software),
        set_fullscreen: Some(win_set_fullscreen),
        toggle_fullscreen: Some(win_toggle_fullscreen),
        begin_transition: Some(win_begin_transition),
        end_transition: Some(win_end_transition),
        in_transition: Some(win_in_transition),
        set_expected_size: Some(win_set_expected_size),
        get_scale: Some(win_get_scale),
        get_display_scale: Some(win_get_display_scale),
        query_window_position: Some(win_query_window_position),
        clamp_window_geometry: Some(win_clamp_window_geometry),
        pump: Some(win_pump),
        run_main_loop: None,
        wake_main_loop: None,
        set_cursor: Some(jfn_input_windows_set_cursor),
        set_idle_inhibit: Some(win_set_idle_inhibit),
        set_theme_color: Some(win_set_theme_color),
        shared_texture_supported: true,
        cef_ozone_platform: [0; 32],
        clipboard_read_text_async: Some(win_clipboard_read_text_async),
        open_external_url: Some(win_open_external_url),
    }
}
