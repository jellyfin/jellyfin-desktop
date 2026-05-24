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
    // dwmapi.dll — tints the titlebar to match the app's theme color.
    fn DwmSetWindowAttribute(
        hwnd: *mut c_void,
        attribute: u32,
        pv_attribute: *const c_void,
        cb_attribute: u32,
    ) -> i32;
}

// =====================================================================
// CEF task bouncer — posts SetThreadExecutionState(flags) onto TID_UI so
// the assertion lives on a stable CEF UI thread. Per-thread state is
// released when that thread calls ES_CONTINUOUS alone. Allocates a small
// ref-counted cef_task_t whose execute() runs on TID_UI and self-deletes.
// =====================================================================

use cef_dll_sys::{
    cef_base_ref_counted_t, cef_post_task, cef_task_t, cef_thread_id_t::TID_UI,
};
use std::sync::atomic::AtomicI32;

#[repr(C)]
struct ExecutionStateTask {
    task: cef_task_t,
    ref_count: AtomicI32,
    flags: u32,
}

unsafe extern "C" fn task_add_ref(self_: *mut cef_base_ref_counted_t) {
    let t = self_ as *mut ExecutionStateTask;
    unsafe { (*t).ref_count.fetch_add(1, Ordering::SeqCst) };
}

unsafe extern "C" fn task_release(self_: *mut cef_base_ref_counted_t) -> c_int {
    let t = self_ as *mut ExecutionStateTask;
    let prev = unsafe { (*t).ref_count.fetch_sub(1, Ordering::SeqCst) };
    if prev == 1 {
        let _ = unsafe { Box::from_raw(t) };
        return 1;
    }
    0
}

unsafe extern "C" fn task_has_one_ref(self_: *mut cef_base_ref_counted_t) -> c_int {
    let t = self_ as *mut ExecutionStateTask;
    (unsafe { (*t).ref_count.load(Ordering::SeqCst) } == 1) as c_int
}

unsafe extern "C" fn task_has_at_least_one_ref(self_: *mut cef_base_ref_counted_t) -> c_int {
    let t = self_ as *mut ExecutionStateTask;
    (unsafe { (*t).ref_count.load(Ordering::SeqCst) } >= 1) as c_int
}

unsafe extern "C" fn task_execute(self_: *mut cef_task_t) {
    let t = self_ as *mut ExecutionStateTask;
    let flags = unsafe { (*t).flags };
    unsafe { SetThreadExecutionState(flags) };
}

unsafe extern "C" {
    fn SetThreadExecutionState(flags: u32) -> u32;
}

fn post_execution_state(flags: u32) {
    let boxed = Box::new(ExecutionStateTask {
        task: cef_task_t {
            base: cef_base_ref_counted_t {
                // CEF validates base.size == sizeof(cef_task_t) on Wrap;
                // ExecutionStateTask is a larger wrapper, not the CEF struct.
                size: std::mem::size_of::<cef_task_t>(),
                add_ref: Some(task_add_ref),
                release: Some(task_release),
                has_one_ref: Some(task_has_one_ref),
                has_at_least_one_ref: Some(task_has_at_least_one_ref),
            },
            execute: Some(task_execute),
        },
        ref_count: AtomicI32::new(1),
        flags,
    });
    let raw = Box::into_raw(boxed);
    unsafe { cef_post_task(TID_UI, raw as *mut cef_task_t) };
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
    post_execution_state(flags);
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

// =====================================================================
// Backend impl
// =====================================================================

use jfn_platform_abi::{IdleInhibitLevel, SurfaceHandle};

pub struct WindowsPlatform;

impl Platform for WindowsPlatform {
    fn display(&self) -> DisplayBackend {
        DisplayBackend::Windows
    }

    fn early_init(&self) {
        win_early_init();
    }

    fn init(&self, mpv: *mut c_void) -> bool {
        win_init(mpv)
    }

    fn cleanup(&self) {
        win_cleanup();
    }

    fn alloc_surface(&self) -> SurfaceHandle {
        win_alloc_surface()
    }

    fn free_surface(&self, s: SurfaceHandle) {
        win_free_surface(s);
    }

    fn surface_present(&self, s: SurfaceHandle, info: *const c_void) -> bool {
        win_surface_present(s, info)
    }

    fn surface_present_software(
        &self,
        s: SurfaceHandle,
        _dirty: *const JfnRect,
        _dirty_len: usize,
        _buffer: *const c_void,
        _w: c_int,
        _h: c_int,
    ) -> bool {
        win_surface_present_software(s, _dirty, _dirty_len, _buffer, _w, _h)
    }

    fn surface_resize(
        &self,
        s: SurfaceHandle,
        lw: c_int,
        lh: c_int,
        pw: c_int,
        ph: c_int,
    ) {
        win_surface_resize(s, lw, lh, pw, ph);
    }

    fn surface_set_visible(&self, s: SurfaceHandle, visible: bool) {
        win_surface_set_visible(s, visible);
    }

    fn restack(&self, ordered: *const SurfaceHandle, n: usize) {
        win_restack(ordered, n);
    }

    fn fade_surface(
        &self,
        s: SurfaceHandle,
        sec: f32,
        on_start: Option<unsafe extern "C" fn(*mut c_void)>,
        start_ctx: *mut c_void,
        start_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
        on_done: Option<unsafe extern "C" fn(*mut c_void)>,
        done_ctx: *mut c_void,
        done_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    ) {
        win_fade_surface(s, sec, on_start, start_ctx, start_dtor, on_done, done_ctx, done_dtor);
    }

    fn popup_show(&self, s: SurfaceHandle, req: *const JfnPopupRequest) {
        win_popup_show(s, req);
    }

    fn popup_hide(&self, s: SurfaceHandle) {
        win_popup_hide(s);
    }

    fn popup_present(
        &self,
        s: SurfaceHandle,
        info: *const c_void,
        lw: c_int,
        lh: c_int,
    ) {
        win_popup_present(s, info, lw, lh);
    }

    fn popup_present_software(
        &self,
        s: SurfaceHandle,
        buffer: *const c_void,
        pw: c_int,
        ph: c_int,
        lw: c_int,
        lh: c_int,
    ) {
        win_popup_present_software(s, buffer, pw, ph, lw, lh);
    }

    fn set_fullscreen(&self, v: bool) {
        win_set_fullscreen(v);
    }

    fn toggle_fullscreen(&self) {
        win_toggle_fullscreen();
    }

    fn begin_transition(&self) {
        win_begin_transition();
    }

    fn end_transition(&self) {
        win_end_transition();
    }

    fn in_transition(&self) -> bool {
        win_in_transition()
    }

    fn set_expected_size(&self, w: c_int, h: c_int) {
        win_set_expected_size(w, h);
    }

    fn get_scale(&self) -> f32 {
        win_get_scale()
    }

    fn get_display_scale(&self, x: c_int, y: c_int) -> f32 {
        win_get_display_scale(x, y)
    }

    fn query_window_position(&self, x: *mut c_int, y: *mut c_int) -> bool {
        win_query_window_position(x, y)
    }

    fn clamp_window_geometry(
        &self,
        w: *mut c_int,
        h: *mut c_int,
        x: *mut c_int,
        y: *mut c_int,
    ) {
        win_clamp_window_geometry(w, h, x, y);
    }

    fn pump(&self) {
        win_pump();
    }

    fn set_cursor(&self, t: c_int) {
        jfn_input_windows_set_cursor(t);
    }

    fn set_idle_inhibit(&self, level: IdleInhibitLevel) {
        win_set_idle_inhibit(level as c_int);
    }

    fn set_theme_color(&self, rgb: u32) {
        win_set_theme_color(rgb);
    }

    fn clipboard_read_text_async(
        &self,
        on_done: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
        ctx: *mut c_void,
        dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    ) {
        win_clipboard_read_text_async(on_done, ctx, dtor);
    }

    fn open_external_url(&self, utf8: *const c_char, len: usize) {
        win_open_external_url(utf8, len);
    }
}

pub fn make_windows_platform() -> Box<dyn Platform> {
    Box::new(WindowsPlatform)
}
