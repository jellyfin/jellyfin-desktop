//! Rust author of the X11 `Platform` vtable.
//!
//! Mirrors `struct Platform` from `src/platform/platform.h` byte-for-byte
//! (`#[repr(C)]`) and exports `make_x11_platform()` for the C++ side.
//! Layout is pinned by static_asserts in `src/platform/platform_ops.cpp`.

#![allow(non_snake_case)]

use std::ffi::{c_char, c_int, c_void};

use crate::surface::{
    JfnRect, jfn_x11_alloc_surface, jfn_x11_fade_surface, jfn_x11_free_surface,
    jfn_x11_restack, jfn_x11_surface_present, jfn_x11_surface_present_software,
    jfn_x11_surface_resize, jfn_x11_surface_set_visible,
};

#[repr(i32)]
#[derive(Copy, Clone)]
pub enum DisplayBackend {
    Wayland = 0,
    X11 = 1,
    Windows = 2,
    MacOS = 3,
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
    pub surface_set_visible: Option<unsafe extern "C" fn(*mut c_void, bool)>,
    pub restack: Option<unsafe extern "C" fn(*const *mut c_void, usize)>,
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
    pub popup_show: Option<unsafe extern "C" fn(*mut c_void, *const JfnPopupRequest)>,
    pub popup_hide: Option<unsafe extern "C" fn(*mut c_void)>,
    pub popup_present:
        Option<unsafe extern "C" fn(*mut c_void, *const c_void, c_int, c_int)>,
    pub popup_present_software: Option<
        unsafe extern "C" fn(*mut c_void, *const c_void, c_int, c_int, c_int, c_int),
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
    pub clamp_window_geometry:
        Option<unsafe extern "C" fn(*mut c_int, *mut c_int, *mut c_int, *mut c_int)>,
    pub pump: Option<unsafe extern "C" fn()>,
    pub run_main_loop: Option<unsafe extern "C" fn()>,
    pub wake_main_loop: Option<unsafe extern "C" fn()>,
    pub set_cursor: Option<unsafe extern "C" fn(c_int)>,
    pub set_idle_inhibit: Option<unsafe extern "C" fn(c_int)>,
    pub set_theme_color: Option<unsafe extern "C" fn(u32)>,
    pub shared_texture_supported: bool,
    pub cef_ozone_platform: [c_char; 32],
    pub clipboard_read_text_async: Option<
        unsafe extern "C" fn(
            Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
            *mut c_void,
            Option<unsafe extern "C" fn(*mut c_void)>,
        ),
    >,
    pub open_external_url: Option<unsafe extern "C" fn(*const c_char, usize)>,
}

unsafe extern "C" {
    fn jfn_idle_inhibit_set(level: u32);
    fn jfn_open_url(url: *const c_char);
    fn jfn_mpv_handle_get() -> *mut c_void;
    fn jfn_mpv_set_fullscreen(v: bool);
    fn jfn_mpv_toggle_fullscreen();
    fn jfn_playback_fullscreen() -> bool;
    fn jfn_playback_display_scale() -> f64;
}

unsafe extern "C" fn early_init() {}

unsafe extern "C" fn init(_mpv: *mut c_void) -> bool {
    crate::lifecycle::init()
}

unsafe extern "C" fn cleanup() {
    crate::lifecycle::cleanup();
}

unsafe extern "C" fn alloc_surface() -> *mut c_void {
    jfn_x11_alloc_surface() as *mut c_void
}

unsafe extern "C" fn free_surface(s: *mut c_void) {
    unsafe {
        jfn_x11_free_surface(s as *mut crate::x11_state::PlatformSurface);
    }
}

unsafe extern "C" fn surface_present(s: *mut c_void, info: *const c_void) -> bool {
    jfn_x11_surface_present(s as *mut crate::x11_state::PlatformSurface, info)
}

unsafe extern "C" fn surface_present_software(
    s: *mut c_void,
    dirty: *const JfnRect,
    dirty_len: usize,
    buffer: *const c_void,
    w: c_int,
    h: c_int,
) -> bool {
    unsafe {
        jfn_x11_surface_present_software(
            s as *mut crate::x11_state::PlatformSurface,
            dirty,
            dirty_len,
            buffer,
            w,
            h,
        )
    }
}

unsafe extern "C" fn surface_resize(
    s: *mut c_void,
    lw: c_int,
    lh: c_int,
    pw: c_int,
    ph: c_int,
) {
    unsafe {
        jfn_x11_surface_resize(
            s as *mut crate::x11_state::PlatformSurface,
            lw,
            lh,
            pw,
            ph,
        );
    }
}

unsafe extern "C" fn surface_set_visible(s: *mut c_void, visible: bool) {
    unsafe {
        jfn_x11_surface_set_visible(
            s as *mut crate::x11_state::PlatformSurface,
            visible,
        );
    }
}

unsafe extern "C" fn restack(handles: *const *mut c_void, n: usize) {
    unsafe {
        jfn_x11_restack(
            handles as *const *mut crate::x11_state::PlatformSurface,
            n,
        );
    }
}

unsafe extern "C" fn fade_surface(
    s: *mut c_void,
    fade_sec: f32,
    on_start: Option<unsafe extern "C" fn(*mut c_void)>,
    start_ctx: *mut c_void,
    start_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    on_done: Option<unsafe extern "C" fn(*mut c_void)>,
    done_ctx: *mut c_void,
    done_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
) {
    unsafe {
        jfn_x11_fade_surface(
            s as *mut crate::x11_state::PlatformSurface,
            fade_sec,
            on_start,
            start_ctx,
            start_dtor,
            on_done,
            done_ctx,
            done_dtor,
        );
    }
}

unsafe extern "C" fn popup_show(_s: *mut c_void, req: *const JfnPopupRequest) {
    if !req.is_null() {
        let r = unsafe { &*req };
        if let Some(d) = r.on_selected_dtor {
            unsafe { d(r.on_selected_ctx) };
        }
    }
}

unsafe extern "C" fn popup_hide(_s: *mut c_void) {}
unsafe extern "C" fn popup_present(
    _s: *mut c_void,
    _info: *const c_void,
    _lw: c_int,
    _lh: c_int,
) {
}
unsafe extern "C" fn popup_present_software(
    _s: *mut c_void,
    _buffer: *const c_void,
    _pw: c_int,
    _ph: c_int,
    _lw: c_int,
    _lh: c_int,
) {
}

unsafe extern "C" fn set_fullscreen(fullscreen: bool) {
    if unsafe { jfn_mpv_handle_get() }.is_null() {
        return;
    }
    if unsafe { jfn_playback_fullscreen() } == fullscreen {
        return;
    }
    unsafe { jfn_mpv_set_fullscreen(fullscreen) };
}

unsafe extern "C" fn toggle_fullscreen() {
    if !unsafe { jfn_mpv_handle_get() }.is_null() {
        unsafe { jfn_mpv_toggle_fullscreen() };
    }
}

unsafe extern "C" fn begin_transition() {}
unsafe extern "C" fn end_transition() {}
unsafe extern "C" fn in_transition() -> bool {
    false
}
unsafe extern "C" fn set_expected_size(_w: c_int, _h: c_int) {}

unsafe extern "C" fn get_scale() -> f32 {
    let s = unsafe { jfn_playback_display_scale() };
    let cached_default = 1.0f32;
    if s > 0.0 {
        let f = s as f32;
        if let Ok(mut g) = crate::x11_state::MUT.lock() {
            if let Some(m) = g.as_mut() {
                m.cached_scale = f;
            }
        }
        return f;
    }
    if let Ok(g) = crate::x11_state::MUT.lock() {
        if let Some(m) = g.as_ref() {
            if m.cached_scale > 0.0 {
                return m.cached_scale;
            }
        }
    }
    cached_default
}

unsafe extern "C" fn get_display_scale(_x: c_int, _y: c_int) -> f32 {
    1.0
}

unsafe extern "C" fn query_window_position(x: *mut c_int, y: *mut c_int) -> bool {
    let Some(conn) = crate::x11_state::conn() else {
        return false;
    };
    let g = crate::x11_state::MUT.lock().unwrap();
    let Some(m) = g.as_ref() else { return false };
    let Some((px, py, _, _)) = crate::lifecycle::query_parent_geometry(&conn, m.parent, m.root) else {
        return false;
    };
    unsafe {
        *x = px;
        *y = py;
    }
    true
}

unsafe extern "C" fn clamp_window_geometry(
    w: *mut c_int,
    h: *mut c_int,
    _x: *mut c_int,
    _y: *mut c_int,
) {
    if w.is_null() || h.is_null() {
        return;
    }
    let mut iw = unsafe { *w };
    let mut ih = unsafe { *h };
    crate::lifecycle::clamp_window_geometry(&mut iw, &mut ih);
    unsafe {
        *w = iw;
        *h = ih;
    }
}

unsafe extern "C" fn pump() {}

unsafe extern "C" fn set_cursor(t: c_int) {
    crate::input_lifecycle::set_cursor_active(t as u32);
}

unsafe extern "C" fn set_idle_inhibit(level: c_int) {
    unsafe { jfn_idle_inhibit_set(level as u32) };
}

unsafe extern "C" fn set_theme_color(_rgb: u32) {}

unsafe extern "C" fn open_external_url(utf8: *const c_char, _len: usize) {
    if !utf8.is_null() {
        unsafe { jfn_open_url(utf8) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn make_x11_platform() -> Platform {
    Platform {
        display: DisplayBackend::X11,
        early_init: Some(early_init),
        init: Some(init),
        cleanup: Some(cleanup),
        post_window_cleanup: None,
        alloc_surface: Some(alloc_surface),
        free_surface: Some(free_surface),
        surface_present: Some(surface_present),
        surface_present_software: Some(surface_present_software),
        surface_resize: Some(surface_resize),
        surface_set_visible: Some(surface_set_visible),
        restack: Some(restack),
        fade_surface: Some(fade_surface),
        popup_show: Some(popup_show),
        popup_hide: Some(popup_hide),
        popup_present: Some(popup_present),
        popup_present_software: Some(popup_present_software),
        set_fullscreen: Some(set_fullscreen),
        toggle_fullscreen: Some(toggle_fullscreen),
        begin_transition: Some(begin_transition),
        end_transition: Some(end_transition),
        in_transition: Some(in_transition),
        set_expected_size: Some(set_expected_size),
        get_scale: Some(get_scale),
        get_display_scale: Some(get_display_scale),
        query_window_position: Some(query_window_position),
        clamp_window_geometry: Some(clamp_window_geometry),
        pump: Some(pump),
        run_main_loop: None,
        wake_main_loop: None,
        set_cursor: Some(set_cursor),
        set_idle_inhibit: Some(set_idle_inhibit),
        set_theme_color: Some(set_theme_color),
        shared_texture_supported: false,
        cef_ozone_platform: [0; 32],
        clipboard_read_text_async: None,
        open_external_url: Some(open_external_url),
    }
}
