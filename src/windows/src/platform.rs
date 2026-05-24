//! Windows platform impl: window init/cleanup, fullscreen toggle
//! helpers, scale + geometry queries, and the WndProc hook that drives
//! compositor resize + transition bookkeeping.
//!
//! All `g_win` state (HWND, cached scale, fullscreen bookkeeping, the
//! WndProc hook handle, the input thread JoinHandle) lives in this
//! module behind a `Mutex<WinState>`.

#![allow(non_snake_case)]

use std::ffi::{c_int, c_void};
use parking_lot::Mutex;
use std::thread::JoinHandle;

use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows::Win32::Graphics::Dwm::DwmExtendFrameIntoClientArea;
use windows::Win32::Graphics::Gdi::{
    GetMonitorInfoW, HMONITOR, MONITOR_DEFAULTTONEAREST, MONITORINFO, MonitorFromWindow,
};
use windows::Win32::UI::Controls::MARGINS;
use windows::Win32::UI::HiDpi::GetDpiForSystem;
use windows::Win32::UI::WindowsAndMessaging::{
    CWPRETSTRUCT, CallNextHookEx, GWL_STYLE, GetWindowLongPtrW, GetWindowRect,
    GetWindowThreadProcessId, HHOOK, IsIconic, IsZoomed, SIZE_MINIMIZED, SPI_GETWORKAREA,
    SYSTEM_PARAMETERS_INFO_UPDATE_FLAGS, SetWindowsHookExW, SystemParametersInfoW,
    UnhookWindowsHookEx, WH_CALLWNDPROCRET, WM_CLOSE, WM_SIZE, WS_CAPTION, WS_THICKFRAME,
};

use jfn_mpv::api::{
    jfn_mpv_get_property_int, jfn_mpv_set_fullscreen, jfn_mpv_set_window_maximized,
    jfn_mpv_set_window_minimized, jfn_mpv_toggle_fullscreen,
};
use jfn_mpv::boot::jfn_mpv_handle_get;
use jfn_playback::ingest_driver::{jfn_playback_display_scale, jfn_playback_fullscreen};
use jfn_playback::shutdown::jfn_shutdown_initiate;

// Input thread lives in `crate::input`.
use crate::input::{
    jfn_input_windows_resize_to_parent, jfn_input_windows_run_input_thread,
    jfn_input_windows_stop_input_thread,
};

// =====================================================================
// File-static state — equivalent of the C++ `static WinState g_win`.
// All access is serialized through `STATE.lock()`. HWND / HHOOK are raw
// pointer types; we are the sole writer so the Mutex is sufficient.
// =====================================================================

struct WinState {
    mpv_hwnd_raw: usize,
    cached_scale: f32,

    // Fullscreen-transition bookkeeping read/written by the WndProc and
    // the fullscreen toggle helpers.
    was_fullscreen: bool,
    was_minimized: bool,
    restore_maximized_on_unfullscreen: bool,

    wndproc_hook_raw: usize,
    input_thread: Option<JoinHandle<()>>,
}

impl WinState {
    const fn new() -> Self {
        Self {
            mpv_hwnd_raw: 0,
            cached_scale: 1.0,
            was_fullscreen: false,
            was_minimized: false,
            restore_maximized_on_unfullscreen: false,
            wndproc_hook_raw: 0,
            input_thread: None,
        }
    }
}

static STATE: Mutex<WinState> = Mutex::new(WinState::new());

fn hwnd_from_raw(raw: usize) -> HWND {
    HWND(raw as *mut c_void)
}

// =====================================================================
// Narrow accessors.
// =====================================================================

/// `jfn_win_get_hwnd` — returns mpv's HWND; nullptr before win_init or
/// after cleanup.
pub fn jfn_win_get_hwnd() -> *mut c_void {
    STATE.lock().mpv_hwnd_raw as *mut c_void
}

fn is_fullscreen_style(style: isize) -> bool {
    let s = style as u32;
    (s & WS_CAPTION.0) == 0 && (s & WS_THICKFRAME.0) == 0
}

// =====================================================================
// Scale + content-size lookups.
// =====================================================================

pub fn win_get_scale() -> f32 {
    let scale = unsafe { jfn_playback_display_scale() };
    if scale > 0.0 {
        let s = scale as f32;
        STATE.lock().cached_scale = s;
        return s;
    }
    let cached = STATE.lock().cached_scale;
    if cached > 0.0 {
        return cached;
    }
    // Pre-mpv (default-geometry sizing at startup): ask the OS directly.
    let dpi = unsafe { GetDpiForSystem() };
    if dpi > 0 { dpi as f32 / 96.0 } else { 1.0 }
}

// Per-monitor DPI (GetDpiForMonitor) lives in Shcore.dll which isn't
// currently linked; fall back to system DPI and ignore (x, y).
pub fn win_get_display_scale(_x: c_int, _y: c_int) -> f32 {
    let dpi = unsafe { GetDpiForSystem() };
    if dpi > 0 { dpi as f32 / 96.0 } else { 1.0 }
}

// =====================================================================
// Fullscreen toggle helpers.
// =====================================================================

fn end_transition_if_settled(target_fullscreen: bool) {
    if crate::win_in_transition() {
        let was_fs = STATE.lock().was_fullscreen;
        if target_fullscreen == was_fs {
            crate::compositor::jfn_win_wndproc_end_transition_locked();
        }
    }
}

pub fn win_set_fullscreen(fullscreen: bool) {
    if unsafe { jfn_mpv_handle_get() }.is_null() {
        return;
    }
    if unsafe { jfn_playback_fullscreen() } == fullscreen {
        end_transition_if_settled(fullscreen);
        return;
    }

    let hwnd_raw = STATE.lock().mpv_hwnd_raw;
    let hwnd = hwnd_from_raw(hwnd_raw);

    if fullscreen {
        STATE.lock().restore_maximized_on_unfullscreen =
            unsafe { IsZoomed(hwnd) }.as_bool();
    }

    let mut should_restore_maximized = false;
    if !fullscreen {
        let mut st = STATE.lock();
        should_restore_maximized = st.restore_maximized_on_unfullscreen;
        st.restore_maximized_on_unfullscreen = false;
    }

    let is_minimized_now = unsafe { IsIconic(hwnd) }.as_bool();
    if !is_minimized_now {
        crate::compositor::jfn_win_wndproc_begin_transition_locked();
    }

    if fullscreen {
        unsafe { jfn_mpv_set_window_minimized(false) };
    }

    unsafe { jfn_mpv_set_fullscreen(fullscreen) };

    if !fullscreen && should_restore_maximized {
        unsafe { jfn_mpv_set_window_maximized(true) };
    }
}

pub fn win_toggle_fullscreen() {
    if unsafe { jfn_mpv_handle_get() }.is_null() {
        return;
    }
    let target_fullscreen = !unsafe { jfn_playback_fullscreen() };

    let hwnd_raw = STATE.lock().mpv_hwnd_raw;
    let hwnd = hwnd_from_raw(hwnd_raw);

    if target_fullscreen {
        STATE.lock().restore_maximized_on_unfullscreen =
            unsafe { IsZoomed(hwnd) }.as_bool();
    }

    let mut should_restore_maximized = false;
    if !target_fullscreen {
        let mut st = STATE.lock();
        should_restore_maximized = st.restore_maximized_on_unfullscreen;
        st.restore_maximized_on_unfullscreen = false;
    }

    let is_minimized_now = unsafe { IsIconic(hwnd) }.as_bool();
    if !is_minimized_now {
        crate::compositor::jfn_win_wndproc_begin_transition_locked();
    }

    if target_fullscreen {
        unsafe { jfn_mpv_set_window_minimized(false) };
    }

    unsafe { jfn_mpv_toggle_fullscreen() };

    if !target_fullscreen && should_restore_maximized {
        unsafe { jfn_mpv_set_window_maximized(true) };
    }
}

// =====================================================================
// WndProc hook.
// =====================================================================

unsafe extern "system" fn mpv_wndproc_hook(n_code: c_int, wp: WPARAM, lp: LPARAM) -> LRESULT {
    if n_code >= 0 {
        let msg = unsafe { &*(lp.0 as *const CWPRETSTRUCT) };
        let target_hwnd_raw = STATE.lock().mpv_hwnd_raw;
        if (msg.hwnd.0 as usize) == target_hwnd_raw {
            if msg.message == WM_SIZE {
                if msg.wParam.0 == SIZE_MINIMIZED as usize {
                    STATE.lock().was_minimized = true;
                    let hook_raw = STATE.lock().wndproc_hook_raw;
                    let hook = HHOOK(hook_raw as *mut c_void);
                    return unsafe { CallNextHookEx(Some(hook), n_code, wp, lp) };
                }

                let lparam = msg.lParam.0 as u32;
                let pw = (lparam & 0xFFFF) as c_int;
                let ph = ((lparam >> 16) & 0xFFFF) as c_int;
                if pw > 0 && ph > 0 {
                    unsafe { jfn_input_windows_resize_to_parent(pw, ph) };

                    let cached = STATE.lock().cached_scale;
                    let scale = if cached > 0.0 { cached } else { 1.0 };
                    let lw = (pw as f32 / scale) as c_int;
                    let lh = (ph as f32 / scale) as c_int;

                    let style =
                        unsafe { GetWindowLongPtrW(hwnd_from_raw(target_hwnd_raw), GWL_STYLE) };
                    let fs = is_fullscreen_style(style);

                    let recovering_from_minimize = STATE.lock().was_minimized;
                    if recovering_from_minimize {
                        {
                            let mut st = STATE.lock();
                            st.was_minimized = false;
                            st.was_fullscreen = fs;
                        }
                        if crate::win_in_transition() {
                            crate::compositor::jfn_win_wndproc_end_transition_locked();
                        }
                    } else {
                        let was_fs = STATE.lock().was_fullscreen;
                        if fs != was_fs {
                            if !crate::win_in_transition() {
                                crate::compositor::jfn_win_wndproc_begin_transition_locked();
                            } else {
                                crate::compositor::jfn_win_wndproc_end_transition_locked();
                            }
                            STATE.lock().was_fullscreen = fs;
                        } else if crate::win_in_transition() {
                            crate::compositor::jfn_win_wndproc_end_transition_locked();
                        }
                    }
                    crate::compositor::jfn_win_update_surface_size(lw, lh, pw, ph);
                }
            } else if msg.message == WM_CLOSE {
                jfn_shutdown_initiate();
            }
        }
    }
    let hook_raw = STATE.lock().wndproc_hook_raw;
    let hook = HHOOK(hook_raw as *mut c_void);
    unsafe { CallNextHookEx(Some(hook), n_code, wp, lp) }
}

// =====================================================================
// Platform vtable entry points.
// =====================================================================

pub fn win_early_init() {
    // Nothing needed on Windows before mpv starts.
}

pub fn win_init(_mpv: *mut c_void) -> bool {
    let mut wid: i64 = 0;
    let name = c"window-id";
    let rc = unsafe { jfn_mpv_get_property_int(name.as_ptr(), &mut wid) };
    if rc < 0 || wid == 0 {
        tracing::error!("Failed to get window-id from mpv");
        return false;
    }
    let hwnd_raw = wid as usize;
    STATE.lock().mpv_hwnd_raw = hwnd_raw;

    // Seed cached_scale.
    win_get_scale();

    // Enable DWM transparency so DComp visuals with premultiplied alpha work.
    let margins = MARGINS {
        cxLeftWidth: -1,
        cxRightWidth: -1,
        cyTopHeight: -1,
        cyBottomHeight: -1,
    };
    unsafe {
        let _ = DwmExtendFrameIntoClientArea(hwnd_from_raw(hwnd_raw), &margins);
    }

    if !crate::compositor::jfn_win_init_compositor(hwnd_raw as *mut c_void) {
        return false;
    }

    // Seed was_fullscreen before installing the hook so the first WM_SIZE
    // doesn't start a spurious transition if already fullscreen.
    {
        let style = unsafe { GetWindowLongPtrW(hwnd_from_raw(hwnd_raw), GWL_STYLE) };
        STATE.lock().was_fullscreen = is_fullscreen_style(style);
    }

    let mpv_tid = unsafe { GetWindowThreadProcessId(hwnd_from_raw(hwnd_raw), None) };
    let hook =
        unsafe { SetWindowsHookExW(WH_CALLWNDPROCRET, Some(mpv_wndproc_hook), None, mpv_tid) };
    match hook {
        Ok(h) => STATE.lock().wndproc_hook_raw = h.0 as usize,
        Err(e) => {
            tracing::error!("SetWindowsHookExW(WH_CALLWNDPROCRET) failed: {e:?}");
            return false;
        }
    }

    let mpv_hwnd_for_thread = hwnd_raw;
    let join = std::thread::spawn(move || {
        unsafe { jfn_input_windows_run_input_thread(mpv_hwnd_for_thread as *mut c_void) };
    });
    STATE.lock().input_thread = Some(join);

    tracing::info!("Windows DirectComposition compositor initialized");
    true
}

pub fn win_cleanup() {
    unsafe { jfn_input_windows_stop_input_thread() };
    let join = STATE.lock().input_thread.take();
    if let Some(j) = join {
        let _ = j.join();
    }
    let hook_raw = STATE.lock().wndproc_hook_raw;
    if hook_raw != 0 {
        let hook = HHOOK(hook_raw as *mut c_void);
        unsafe {
            let _ = UnhookWindowsHookEx(hook);
        }
        STATE.lock().wndproc_hook_raw = 0;
    }

    crate::compositor::jfn_win_cleanup_compositor();

    STATE.lock().mpv_hwnd_raw = 0;
}

// =====================================================================
// Window-position / geometry helpers.
// =====================================================================

/// Query window position relative to the monitor's working area (excludes
/// taskbar), in physical pixels. Matches mpv's `--geometry +X+Y`
/// coordinate system on Windows (`vo_calc_window_geometry` uses the
/// working area).
pub fn win_query_window_position(x: &mut c_int, y: &mut c_int) -> bool {
    let hwnd_raw = STATE.lock().mpv_hwnd_raw;
    if hwnd_raw == 0 {
        return false;
    }
    let hwnd = hwnd_from_raw(hwnd_raw);
    let mut wr = RECT::default();
    if unsafe { GetWindowRect(hwnd, &mut wr) }.is_err() {
        return false;
    }
    let mon: HMONITOR = unsafe { MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) };
    let mut mi = MONITORINFO {
        cbSize: std::mem::size_of::<MONITORINFO>() as u32,
        ..Default::default()
    };
    if !unsafe { GetMonitorInfoW(mon, &mut mi) }.as_bool() {
        return false;
    }
    *x = wr.left - mi.rcWork.left;
    *y = wr.top - mi.rcWork.top;
    true
}

/// Resolve saved geometry against the primary monitor's working area so the
/// window never opens larger than the screen or off-screen, and center any
/// unset axis.
pub fn win_clamp_window_geometry(
    w: &mut c_int,
    h: &mut c_int,
    x: &mut c_int,
    y: &mut c_int,
) {
    let mut work = RECT::default();
    let ok = unsafe {
        SystemParametersInfoW(
            SPI_GETWORKAREA,
            0,
            Some(&mut work as *mut RECT as *mut c_void),
            SYSTEM_PARAMETERS_INFO_UPDATE_FLAGS(0),
        )
    };
    if ok.is_err() {
        return;
    }
    let vw = work.right - work.left;
    let vh = work.bottom - work.top;
    if *w > vw {
        *w = vw;
    }
    if *h > vh {
        *h = vh;
    }
    if *x < 0 {
        *x = (vw - *w) / 2;
    }
    if *y < 0 {
        *y = (vh - *h) / 2;
    }
    if *x + *w > vw {
        *x = vw - *w;
    }
    if *y + *h > vh {
        *y = vh - *h;
    }
    if *x < 0 {
        *x = 0;
    }
    if *y < 0 {
        *y = 0;
    }
}
