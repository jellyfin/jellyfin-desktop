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
use std::sync::atomic::{AtomicBool, Ordering};

pub use jfn_platform_abi::{DisplayBackend, JfnPopupRequest, JfnRect, Platform};

mod compositor;
mod input;
mod platform;
pub use input::{
    jfn_input_windows_resize_to_parent, jfn_input_windows_run_input_thread,
    jfn_input_windows_set_cursor, jfn_input_windows_stop_input_thread,
};
pub use compositor::{
    jfn_win_begin_transition_locked, jfn_win_cleanup_compositor, jfn_win_init_compositor,
    jfn_win_update_surface_size, jfn_win_wndproc_begin_transition_locked,
    jfn_win_wndproc_end_transition_locked, win_alloc_surface, win_end_transition,
    win_fade_surface, win_free_surface, win_popup_hide, win_popup_present,
    win_popup_present_software, win_popup_show, win_restack, win_set_expected_size,
    win_surface_present, win_surface_present_software, win_surface_resize,
    win_surface_set_visible,
};
pub use platform::{
    jfn_win_get_hwnd, win_clamp_window_geometry, win_cleanup, win_early_init,
    win_get_display_scale, win_get_scale, win_init, win_query_window_position,
    win_set_fullscreen, win_toggle_fullscreen,
};

#[unsafe(no_mangle)]
pub extern "C" fn win_pump() {
    // Input handled by dedicated input-thread message loop.
}

// =====================================================================
// State-bound bodies ported to native Rust.
// =====================================================================

unsafe extern "C" {
    /// C++ helper that posts a SetThreadExecutionState(flags) call onto
    /// TID_UI so the assertion lives on a stable CEF UI thread. Per-thread
    /// state is released when that thread calls ES_CONTINUOUS alone.
    fn jfn_win_post_execution_state(flags: u32);

    // dwmapi.dll — tints the titlebar to match the app's theme color.
    fn DwmSetWindowAttribute(
        hwnd: *mut c_void,
        attribute: u32,
        pv_attribute: *const c_void,
        cb_attribute: u32,
    ) -> i32;
}

const DWMWA_CAPTION_COLOR: u32 = 35;

// SetThreadExecutionState flags (winbase.h).
const ES_CONTINUOUS: u32 = 0x8000_0000;
const ES_SYSTEM_REQUIRED: u32 = 0x0000_0001;
const ES_DISPLAY_REQUIRED: u32 = 0x0000_0002;

/// Tint the DWM titlebar so it matches the current theme color.
/// rgb is 0x00RRGGBB; DWMWA_CAPTION_COLOR wants 0x00BBGGRR (COLORREF).
#[unsafe(no_mangle)]
pub extern "C" fn win_set_theme_color(rgb: u32) {
    let hwnd = unsafe { jfn_win_get_hwnd() };
    if hwnd.is_null() {
        return;
    }
    let r = (rgb >> 16) & 0xFF;
    let g = (rgb >> 8) & 0xFF;
    let b = rgb & 0xFF;
    let colorref: u32 = r | (g << 8) | (b << 16);
    unsafe {
        DwmSetWindowAttribute(
            hwnd,
            DWMWA_CAPTION_COLOR,
            &colorref as *const u32 as *const c_void,
            std::mem::size_of::<u32>() as u32,
        );
    }
}

/// Map IdleInhibitLevel (None=0, System=1, Display=2) to execution-state
/// flags and post the call onto TID_UI so it lives on a stable thread.
#[unsafe(no_mangle)]
pub extern "C" fn win_set_idle_inhibit(level: c_int) {
    let mut flags = ES_CONTINUOUS;
    match level {
        2 => flags |= ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED,
        1 => flags |= ES_SYSTEM_REQUIRED,
        _ => {}
    }
    unsafe { jfn_win_post_execution_state(flags) };
}

// =====================================================================
// Fullscreen-transition gating flag. Read by win_surface_present each
// frame (under STATE lock in compositor.rs); set/cleared by the Rust
// begin_transition_locked / end_transition_locked helpers. SeqCst
// matches the prior plain-bool semantics with no surrounding ordering
// requirements.
// =====================================================================

pub(crate) static G_TRANSITIONING: AtomicBool = AtomicBool::new(false);

#[unsafe(no_mangle)]
pub extern "C" fn win_begin_transition() {
    jfn_win_begin_transition_locked();
}

#[unsafe(no_mangle)]
pub extern "C" fn win_in_transition() -> bool {
    G_TRANSITIONING.load(Ordering::SeqCst)
}

// =====================================================================
// Clipboard (Win32 CF_UNICODETEXT) — read only; writes go through CEF's
// own frame->Copy() path which works correctly on Windows. Win32
// clipboard is synchronous; callback fires inline on the calling thread.
// =====================================================================

const CF_UNICODETEXT: u32 = 13;
const CP_UTF8: u32 = 65001;
const SW_SHOWNORMAL: c_int = 1;

unsafe extern "C" {
    fn OpenClipboard(hwnd: *mut c_void) -> i32;
    fn CloseClipboard() -> i32;
    fn GetClipboardData(format: u32) -> *mut c_void;
    fn GlobalLock(h: *mut c_void) -> *mut c_void;
    fn GlobalUnlock(h: *mut c_void) -> i32;
    fn WideCharToMultiByte(
        code_page: u32,
        flags: u32,
        wide: *const u16,
        wide_len: c_int,
        out: *mut u8,
        out_len: c_int,
        default_char: *const u8,
        used_default: *mut i32,
    ) -> c_int;
    fn MultiByteToWideChar(
        code_page: u32,
        flags: u32,
        input: *const u8,
        input_len: c_int,
        out: *mut u16,
        out_len: c_int,
    ) -> c_int;
    fn ShellExecuteW(
        hwnd: *mut c_void,
        verb: *const u16,
        file: *const u16,
        params: *const u16,
        dir: *const u16,
        show_cmd: c_int,
    ) -> *mut c_void;
}

#[unsafe(no_mangle)]
pub extern "C" fn win_clipboard_read_text_async(
    on_done: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
    ctx: *mut c_void,
    dtor: Option<unsafe extern "C" fn(*mut c_void)>,
) {
    let mut result: Vec<u8> = Vec::new();
    unsafe {
        if OpenClipboard(std::ptr::null_mut()) != 0 {
            let h = GetClipboardData(CF_UNICODETEXT);
            if !h.is_null() {
                let wbuf = GlobalLock(h) as *const u16;
                if !wbuf.is_null() {
                    let bytes = WideCharToMultiByte(
                        CP_UTF8,
                        0,
                        wbuf,
                        -1,
                        std::ptr::null_mut(),
                        0,
                        std::ptr::null(),
                        std::ptr::null_mut(),
                    );
                    if bytes > 1 {
                        // bytes includes the terminating NUL.
                        result.resize((bytes - 1) as usize, 0);
                        WideCharToMultiByte(
                            CP_UTF8,
                            0,
                            wbuf,
                            -1,
                            result.as_mut_ptr(),
                            bytes,
                            std::ptr::null(),
                            std::ptr::null_mut(),
                        );
                    }
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
    }
    if let Some(cb) = on_done {
        unsafe { cb(ctx, result.as_ptr() as *const c_char, result.len()) };
    }
    if let Some(d) = dtor {
        unsafe { d(ctx) };
    }
}

/// Open an external URL via `ShellExecuteW(open)`.
#[unsafe(no_mangle)]
pub extern "C" fn win_open_external_url(utf8: *const c_char, len: usize) {
    if utf8.is_null() || len == 0 {
        return;
    }
    let wlen = unsafe {
        MultiByteToWideChar(CP_UTF8, 0, utf8 as *const u8, len as c_int, std::ptr::null_mut(), 0)
    };
    if wlen <= 0 {
        return;
    }
    let mut wurl: Vec<u16> = vec![0u16; wlen as usize + 1];
    unsafe {
        MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8 as *const u8,
            len as c_int,
            wurl.as_mut_ptr(),
            wlen,
        );
    }
    // NUL-terminate (vec initialised to 0, but be explicit).
    wurl[wlen as usize] = 0;
    let verb: [u16; 5] = [b'o' as u16, b'p' as u16, b'e' as u16, b'n' as u16, 0];
    unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            verb.as_ptr(),
            wurl.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            SW_SHOWNORMAL,
        );
    }
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
