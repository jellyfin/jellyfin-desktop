//! Rust author of the macOS `Platform` vtable.
//!
//! Composition only — individual platform functions (compositor, NSApp
//! lifecycle, idle inhibit, clipboard, etc.) still live in
//! `src/platform/macos.mm`. They are exposed with `extern "C"` linkage so
//! this crate can populate the vtable from them by symbol name. Subsequent
//! slices replace each thunk with a native Rust implementation; the C ABI
//! at the vtable boundary stays stable.
//!
//! Returns the shared `Platform` from `jfn-platform-abi`. Layout is pinned
//! by `static_assert`s in `src/platform/platform_ops.cpp`.

#![cfg(target_os = "macos")]
#![allow(non_snake_case)]

use std::ffi::{c_char, c_int, c_void};

pub use jfn_platform_abi::{DisplayBackend, JfnPopupRequest, JfnRect, Platform};

// =====================================================================
// External C symbols (src/platform/macos.mm + src/input/input_macos.mm)
// =====================================================================

unsafe extern "C" {
    fn macos_early_init();
    fn macos_init(mpv: *mut c_void) -> bool;
    fn macos_cleanup();
    fn macos_alloc_surface() -> *mut c_void;
    fn macos_free_surface(s: *mut c_void);
    fn macos_surface_present(s: *mut c_void, info: *const c_void) -> bool;
    fn macos_surface_present_software(
        s: *mut c_void,
        dirty: *const JfnRect,
        dirty_len: usize,
        buffer: *const c_void,
        w: c_int,
        h: c_int,
    ) -> bool;
    fn macos_surface_resize(s: *mut c_void, lw: c_int, lh: c_int, pw: c_int, ph: c_int);
    fn macos_surface_set_visible(s: *mut c_void, visible: bool);
    fn macos_restack(ordered: *const *mut c_void, n: usize);
    fn macos_fade_surface(
        s: *mut c_void,
        fade_sec: f32,
        on_fade_start: Option<unsafe extern "C" fn(*mut c_void)>,
        start_ctx: *mut c_void,
        start_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
        on_complete: Option<unsafe extern "C" fn(*mut c_void)>,
        done_ctx: *mut c_void,
        done_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    );
    fn macos_popup_show(s: *mut c_void, req: *const JfnPopupRequest);
    fn macos_set_fullscreen(fullscreen: bool);
    fn macos_toggle_fullscreen();
    fn macos_begin_transition();
    fn macos_end_transition();
    fn macos_in_transition() -> bool;
    fn macos_set_expected_size(w: c_int, h: c_int);
    fn macos_get_scale() -> f32;
    fn macos_get_display_scale(x: c_int, y: c_int) -> f32;
    fn macos_query_window_position(x: *mut c_int, y: *mut c_int) -> bool;
    fn macos_clamp_window_geometry(
        w: *mut c_int,
        h: *mut c_int,
        x: *mut c_int,
        y: *mut c_int,
    );
    fn macos_pump();
    fn macos_run_main_loop();
    fn macos_wake_main_loop();
    fn macos_set_idle_inhibit(level: c_int);
    fn macos_set_theme_color(rgb: u32);
    fn macos_clipboard_read_text_async(
        on_done: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
        ctx: *mut c_void,
        dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    );
    fn macos_open_external_url(utf8: *const c_char, len: usize);
    fn jfn_input_macos_set_cursor(t: c_int);
}

// =====================================================================
// Popup no-op trampolines for compositor backends that delegate popups
// to NSMenu (set in macos_popup_show). popup_present / popup_present_
// software / popup_hide are unused on macOS.
// =====================================================================

unsafe extern "C" fn popup_hide_noop(_s: *mut c_void) {}
unsafe extern "C" fn popup_present_noop(
    _s: *mut c_void,
    _info: *const c_void,
    _lw: c_int,
    _lh: c_int,
) {
}
unsafe extern "C" fn popup_present_software_noop(
    _s: *mut c_void,
    _buffer: *const c_void,
    _pw: c_int,
    _ph: c_int,
    _lw: c_int,
    _lh: c_int,
) {
}

// =====================================================================
// Factory
// =====================================================================

/// Returns the macOS `Platform` vtable by value across the C ABI.
/// Called from C++ `make_platform(DisplayBackend::macOS)` (platform.h).
#[unsafe(no_mangle)]
pub extern "C" fn make_macos_platform() -> Platform {
    Platform {
        display: DisplayBackend::MacOS,
        early_init: Some(macos_early_init),
        init: Some(macos_init),
        cleanup: Some(macos_cleanup),
        post_window_cleanup: None,
        alloc_surface: Some(macos_alloc_surface),
        free_surface: Some(macos_free_surface),
        surface_present: Some(macos_surface_present),
        surface_present_software: Some(macos_surface_present_software),
        surface_resize: Some(macos_surface_resize),
        surface_set_visible: Some(macos_surface_set_visible),
        restack: Some(macos_restack),
        fade_surface: Some(macos_fade_surface),
        popup_show: Some(macos_popup_show),
        popup_hide: Some(popup_hide_noop),
        popup_present: Some(popup_present_noop),
        popup_present_software: Some(popup_present_software_noop),
        set_fullscreen: Some(macos_set_fullscreen),
        toggle_fullscreen: Some(macos_toggle_fullscreen),
        begin_transition: Some(macos_begin_transition),
        end_transition: Some(macos_end_transition),
        in_transition: Some(macos_in_transition),
        set_expected_size: Some(macos_set_expected_size),
        get_scale: Some(macos_get_scale),
        get_display_scale: Some(macos_get_display_scale),
        query_window_position: Some(macos_query_window_position),
        clamp_window_geometry: Some(macos_clamp_window_geometry),
        pump: Some(macos_pump),
        run_main_loop: Some(macos_run_main_loop),
        wake_main_loop: Some(macos_wake_main_loop),
        set_cursor: Some(jfn_input_macos_set_cursor),
        set_idle_inhibit: Some(macos_set_idle_inhibit),
        set_theme_color: Some(macos_set_theme_color),
        shared_texture_supported: true,
        cef_ozone_platform: [0; 32],
        clipboard_read_text_async: Some(macos_clipboard_read_text_async),
        open_external_url: Some(macos_open_external_url),
    }
}
