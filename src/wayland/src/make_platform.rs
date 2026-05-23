//! Rust author of the Wayland `Platform` vtable.
//!
//! Mirrors `struct Platform` from `src/platform/platform.h` byte-for-byte
//! (`#[repr(C)]`) and exports `make_wayland_platform()` so the C++ side can
//! consume the factory the same way it does the macOS/Windows/X11 ones.
//!
//! Layout invariants are pinned by `static_assert`s on the C++ side
//! (`src/platform/platform_layout.cpp`) — any drift surfaces at compile
//! time, before bad offsets ever reach a function call.

#![allow(non_snake_case)]

use std::ffi::{c_char, c_int, c_void};

use crate::wl_ops::{self, JfnDmabufFrame};

pub use jfn_platform_abi::{DisplayBackend, JfnPopupRequest, JfnRect, Platform};

// =====================================================================
// External symbols referenced by the trampolines.
// =====================================================================

unsafe extern "C" {
    fn jfn_wl_lifecycle_init() -> bool;
    fn jfn_wl_lifecycle_cleanup();
    #[cfg(feature = "kde-palette")]
    fn jfn_wl_kde_palette_post_window_cleanup();
    #[cfg(feature = "kde-palette")]
    fn jfn_wl_kde_palette_set_color(r: u8, g: u8, b: u8, hex: *const c_char);
    fn jfn_wl_get_cached_scale() -> f32;
    fn jfn_wayland_scale_probe(x: c_int, y: c_int) -> f64;
    fn jfn_idle_inhibit_set(level: u32);
    fn jfn_open_url(url: *const c_char);
    fn jfn_playback_display_hz() -> f64;
    fn jfn_wl_fade_start(
        surface: *mut c_void,
        fade_sec: f32,
        fps: f64,
        apply: unsafe extern "C" fn(*mut c_void, u32) -> bool,
        on_start: Option<unsafe extern "C" fn(*mut c_void)>,
        start_ctx: *mut c_void,
        start_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
        on_done: Option<unsafe extern "C" fn(*mut c_void)>,
        done_ctx: *mut c_void,
        done_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    );
    fn jfn_wl_fade_apply_frame(surface: *mut c_void, alpha: u32) -> bool;
}

// =====================================================================
// Trampolines
// =====================================================================

unsafe extern "C" fn wl_early_init() {}

unsafe extern "C" fn wl_init(_mpv: *mut c_void) -> bool {
    unsafe { jfn_wl_lifecycle_init() }
}

unsafe extern "C" fn wl_cleanup() {
    unsafe { jfn_wl_lifecycle_cleanup() };
}

unsafe extern "C" fn wl_post_window_cleanup() {
    #[cfg(feature = "kde-palette")]
    unsafe { jfn_wl_kde_palette_post_window_cleanup() };
}

unsafe extern "C" fn wl_alloc_surface() -> *mut c_void {
    wl_ops::alloc_surface() as *mut c_void
}

unsafe extern "C" fn wl_free_surface(s: *mut c_void) {
    wl_ops::free_surface(s as *mut crate::wl_state::PlatformSurface);
}

unsafe extern "C" fn wl_restack(handles: *const *mut c_void, n: usize) {
    if handles.is_null() {
        return;
    }
    let slice = unsafe { std::slice::from_raw_parts(handles, n) };
    // `wl_ops::restack` takes a slice of `*mut PlatformSurface`. The void*
    // and PlatformSurface* are the same pointer values.
    let typed: &[*mut crate::wl_state::PlatformSurface] = unsafe {
        std::slice::from_raw_parts(
            slice.as_ptr() as *const *mut crate::wl_state::PlatformSurface,
            n,
        )
    };
    wl_ops::restack(typed);
}

// Unpack a CefAcceleratedPaintInfo into JfnDmabufFrame, dup'ing the fd
// so the caller's wire copy can be closed independently.
unsafe fn to_dmabuf_frame(info: *const c_void) -> Option<JfnDmabufFrame> {
    let info = info as *const cef_dll_sys::_cef_accelerated_paint_info_t;
    if info.is_null() {
        return None;
    }
    let info = unsafe { &*info };
    let plane0 = &info.planes[0];
    let dup_fd = unsafe { libc::dup(plane0.fd) };
    if dup_fd < 0 {
        return None;
    }
    Some(JfnDmabufFrame {
        fd: dup_fd,
        stride: plane0.stride,
        modifier: info.modifier,
        coded_w: info.extra.coded_size.width,
        coded_h: info.extra.coded_size.height,
        visible_w: info.extra.visible_rect.width,
        visible_h: info.extra.visible_rect.height,
    })
}

unsafe extern "C" fn wl_surface_present(
    s: *mut c_void,
    accel_paint_info: *const c_void,
) -> bool {
    let Some(frame) = (unsafe { to_dmabuf_frame(accel_paint_info) }) else {
        return false;
    };
    // The dup'd fd lives for the lifetime of the dmabuf buffer the
    // compositor imports; wayland-client borrows but does not close it.
    // Matches the C++ trampoline that previously dup'd and let CEF's
    // lifecycle stay independent.
    wl_ops::surface_present(
        s as *mut crate::wl_state::PlatformSurface,
        &frame,
    )
}

unsafe extern "C" fn wl_surface_present_software(
    s: *mut c_void,
    _dirty: *const JfnRect,
    _dirty_len: usize,
    buffer: *const c_void,
    w: c_int,
    h: c_int,
) -> bool {
    if buffer.is_null() || w <= 0 || h <= 0 {
        return false;
    }
    let len = (w as usize)
        .checked_mul(h as usize)
        .and_then(|n| n.checked_mul(4));
    let Some(len) = len else { return false };
    let pixels = unsafe { std::slice::from_raw_parts(buffer as *const u8, len) };
    wl_ops::surface_present_software(
        s as *mut crate::wl_state::PlatformSurface,
        pixels,
        w,
        h,
    )
}

unsafe extern "C" fn wl_surface_resize(
    s: *mut c_void,
    lw: c_int,
    lh: c_int,
    pw: c_int,
    ph: c_int,
) {
    wl_ops::surface_resize(
        s as *mut crate::wl_state::PlatformSurface,
        lw,
        lh,
        pw,
        ph,
    );
}

// Background color matches kBgColor (0x101010) on the C++ side. Hard-coded
// here so the Platform vtable doesn't carry the color across the FFI.
const BG_R: u8 = 0x10;
const BG_G: u8 = 0x10;
const BG_B: u8 = 0x10;

unsafe extern "C" fn wl_surface_set_visible(s: *mut c_void, visible: bool) {
    wl_ops::surface_set_visible(
        s as *mut crate::wl_state::PlatformSurface,
        visible,
        BG_R,
        BG_G,
        BG_B,
    );
}

unsafe extern "C" fn wl_fade_surface(
    s: *mut c_void,
    fade_sec: f32,
    on_start: Option<unsafe extern "C" fn(*mut c_void)>,
    start_ctx: *mut c_void,
    start_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    on_done: Option<unsafe extern "C" fn(*mut c_void)>,
    done_ctx: *mut c_void,
    done_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
) {
    let fps = unsafe { jfn_playback_display_hz() };
    let surf_ptr = s as *mut crate::wl_state::PlatformSurface;
    let can_fade = !s.is_null() && fps > 0.0 && wl_ops::surface_has_alpha(surf_ptr);
    if !can_fade {
        // No wp_alpha_modifier_v1 (e.g. niri) or no surface/fps: hard-unmap
        // the subsurface so the splash disappears synchronously, then fire
        // the callback contract inline. Mirrors X11's jfn_x11_fade_surface.
        if !s.is_null() {
            wl_ops::surface_set_visible(surf_ptr, false, BG_R, BG_G, BG_B);
        }
        if let Some(f) = on_start { unsafe { f(start_ctx) } }
        if let Some(d) = start_dtor { unsafe { d(start_ctx) } }
        if let Some(f) = on_done { unsafe { f(done_ctx) } }
        if let Some(d) = done_dtor { unsafe { d(done_ctx) } }
        return;
    }
    unsafe {
        jfn_wl_fade_start(
            s,
            fade_sec,
            fps,
            jfn_wl_fade_apply_frame,
            on_start,
            start_ctx,
            start_dtor,
            on_done,
            done_ctx,
            done_dtor,
        );
    }
}

unsafe extern "C" fn wl_popup_show(
    s: *mut c_void,
    req: *const JfnPopupRequest,
) {
    if req.is_null() {
        return;
    }
    let r = unsafe { &*req };
    wl_ops::popup_show(
        s as *mut crate::wl_state::PlatformSurface,
        r.x,
        r.y,
        r.lw,
        r.lh,
    );
    if let Some(d) = r.on_selected_dtor {
        unsafe { d(r.on_selected_ctx) };
    }
}

unsafe extern "C" fn wl_popup_hide(s: *mut c_void) {
    wl_ops::popup_hide(s as *mut crate::wl_state::PlatformSurface);
}

unsafe extern "C" fn wl_popup_present(
    s: *mut c_void,
    accel_paint_info: *const c_void,
    lw: c_int,
    lh: c_int,
) {
    let Some(frame) = (unsafe { to_dmabuf_frame(accel_paint_info) }) else {
        return;
    };
    wl_ops::popup_present(
        s as *mut crate::wl_state::PlatformSurface,
        &frame,
        lw,
        lh,
    );
}

unsafe extern "C" fn wl_popup_present_software(
    s: *mut c_void,
    buffer: *const c_void,
    pw: c_int,
    ph: c_int,
    lw: c_int,
    lh: c_int,
) {
    if buffer.is_null() || pw <= 0 || ph <= 0 {
        return;
    }
    let len = (pw as usize)
        .checked_mul(ph as usize)
        .and_then(|n| n.checked_mul(4));
    let Some(len) = len else { return };
    let pixels = unsafe { std::slice::from_raw_parts(buffer as *const u8, len) };
    wl_ops::popup_present_software(
        s as *mut crate::wl_state::PlatformSurface,
        pixels,
        pw,
        ph,
        lw,
        lh,
    );
}

unsafe extern "C" fn wl_set_fullscreen(fullscreen: bool) {
    crate::wl_ffi::jfn_wl_set_fullscreen(fullscreen);
}

unsafe extern "C" fn wl_toggle_fullscreen() {
    crate::wl_ffi::jfn_wl_toggle_fullscreen();
}

unsafe extern "C" fn wl_begin_transition() {
    crate::wl_ffi::jfn_wl_begin_transition();
}

unsafe extern "C" fn wl_end_transition() {
    crate::wl_ffi::jfn_wl_end_transition();
}

unsafe extern "C" fn wl_in_transition() -> bool {
    crate::wl_ffi::jfn_wl_in_transition()
}

unsafe extern "C" fn wl_set_expected_size(_w: c_int, _h: c_int) {}

unsafe extern "C" fn wl_pump() {}

unsafe extern "C" fn wl_get_scale() -> f32 {
    unsafe { jfn_wl_get_cached_scale() }
}

unsafe extern "C" fn wl_get_display_scale(x: c_int, y: c_int) -> f32 {
    let s = unsafe { jfn_wayland_scale_probe(x, y) };
    if s > 0.0 { s as f32 } else { 1.0 }
}

unsafe extern "C" fn wl_query_window_position(
    _x: *mut c_int,
    _y: *mut c_int,
) -> bool {
    false
}

unsafe extern "C" fn wl_set_cursor(ty: c_int) {
    crate::input_lifecycle::set_cursor_active(ty as u32);
}

unsafe extern "C" fn wl_set_idle_inhibit(level: c_int) {
    unsafe { jfn_idle_inhibit_set(level as u32) };
}

unsafe extern "C" fn wl_set_theme_color(rgb: u32) {
    #[cfg(feature = "kde-palette")]
    {
        let r = ((rgb >> 16) & 0xFF) as u8;
        let g = ((rgb >> 8) & 0xFF) as u8;
        let b = (rgb & 0xFF) as u8;
        // hex string "#RRGGBB\0" — same layout the C++ Color constructor would
        // produce.
        let mut hex: [u8; 8] = [0; 8];
        hex[0] = b'#';
        let hexdigit = |c: u8| if c < 10 { b'0' + c } else { b'a' + (c - 10) };
        hex[1] = hexdigit((r >> 4) & 0xF);
        hex[2] = hexdigit(r & 0xF);
        hex[3] = hexdigit((g >> 4) & 0xF);
        hex[4] = hexdigit(g & 0xF);
        hex[5] = hexdigit((b >> 4) & 0xF);
        hex[6] = hexdigit(b & 0xF);
        hex[7] = 0;
        unsafe { jfn_wl_kde_palette_set_color(r, g, b, hex.as_ptr() as *const c_char) };
    }
    #[cfg(not(feature = "kde-palette"))]
    let _ = rgb;
}

unsafe extern "C" fn wl_clipboard_read_text_async(
    on_done: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
    ctx: *mut c_void,
    dtor: Option<unsafe extern "C" fn(*mut c_void)>,
) {
    crate::clipboard::clipboard_read_text_async(on_done, ctx, dtor);
}

unsafe extern "C" fn wl_open_external_url(utf8: *const c_char, _len: usize) {
    if !utf8.is_null() {
        unsafe { jfn_open_url(utf8) };
    }
}

// =====================================================================
// Factory
// =====================================================================

#[unsafe(no_mangle)]
pub extern "C" fn make_wayland_platform() -> Platform {
    Platform {
        display: DisplayBackend::Wayland,
        early_init: Some(wl_early_init),
        init: Some(wl_init),
        cleanup: Some(wl_cleanup),
        post_window_cleanup: Some(wl_post_window_cleanup),
        alloc_surface: Some(wl_alloc_surface),
        free_surface: Some(wl_free_surface),
        surface_present: Some(wl_surface_present),
        surface_present_software: Some(wl_surface_present_software),
        surface_resize: Some(wl_surface_resize),
        surface_set_visible: Some(wl_surface_set_visible),
        restack: Some(wl_restack),
        fade_surface: Some(wl_fade_surface),
        popup_show: Some(wl_popup_show),
        popup_hide: Some(wl_popup_hide),
        popup_present: Some(wl_popup_present),
        popup_present_software: Some(wl_popup_present_software),
        set_fullscreen: Some(wl_set_fullscreen),
        toggle_fullscreen: Some(wl_toggle_fullscreen),
        begin_transition: Some(wl_begin_transition),
        end_transition: Some(wl_end_transition),
        in_transition: Some(wl_in_transition),
        set_expected_size: Some(wl_set_expected_size),
        get_scale: Some(wl_get_scale),
        get_display_scale: Some(wl_get_display_scale),
        query_window_position: Some(wl_query_window_position),
        clamp_window_geometry: None,
        pump: Some(wl_pump),
        run_main_loop: None,
        wake_main_loop: None,
        set_cursor: Some(wl_set_cursor),
        set_idle_inhibit: Some(wl_set_idle_inhibit),
        set_theme_color: Some(wl_set_theme_color),
        shared_texture_supported: true,
        cef_ozone_platform: [0; 32],
        clipboard_read_text_async: Some(wl_clipboard_read_text_async),
        open_external_url: Some(wl_open_external_url),
    }
}

