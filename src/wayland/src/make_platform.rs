//! Wayland backend impl of [`jfn_platform_abi::Platform`].
//!
//! Each method forwards to the existing Rust `wl_*` / `jfn_wl_*` helpers
//! (mostly `crate::wl_ops` + `crate::wl_ffi`). The factory returns the
//! concrete type; `jfn_app_main` boxes it as `Box<dyn Platform>` before
//! handing it to `jfn_platform_abi::install`.

#![allow(non_snake_case)]

use std::ffi::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicBool, Ordering};

use crate::wl_ops::{self, JfnDmabufFrame};

pub use jfn_platform_abi::{
    DisplayBackend, IdleInhibitLevel, JfnPopupRequest, JfnRect, Platform, SurfaceHandle,
};

// =====================================================================
// External symbols
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
// Helpers
// =====================================================================

// Background color matches kBgColor (0x101010) on the C++ side. Hard-coded
// here so the surface_set_visible path doesn't need to carry the color.
const BG_R: u8 = 0x10;
const BG_G: u8 = 0x10;
const BG_B: u8 = 0x10;

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

// =====================================================================
// Backend
// =====================================================================

pub struct WaylandPlatform {
    shared_texture: AtomicBool,
    clipboard: AtomicBool,
}

impl WaylandPlatform {
    pub fn new() -> Self {
        Self {
            shared_texture: AtomicBool::new(true),
            clipboard: AtomicBool::new(true),
        }
    }
}

impl Default for WaylandPlatform {
    fn default() -> Self {
        Self::new()
    }
}

impl Platform for WaylandPlatform {
    fn display(&self) -> DisplayBackend {
        DisplayBackend::Wayland
    }

    fn init(&self, _mpv: *mut c_void) -> bool {
        unsafe { jfn_wl_lifecycle_init() }
    }

    fn cleanup(&self) {
        unsafe { jfn_wl_lifecycle_cleanup() };
    }

    fn post_window_cleanup(&self) {
        #[cfg(feature = "kde-palette")]
        unsafe { jfn_wl_kde_palette_post_window_cleanup() };
    }

    fn alloc_surface(&self) -> SurfaceHandle {
        wl_ops::alloc_surface() as *mut c_void
    }

    fn free_surface(&self, s: SurfaceHandle) {
        wl_ops::free_surface(s as *mut crate::wl_state::PlatformSurface);
    }

    fn surface_present(&self, s: SurfaceHandle, info: *const c_void) -> bool {
        let Some(frame) = (unsafe { to_dmabuf_frame(info) }) else {
            return false;
        };
        wl_ops::surface_present(s as *mut crate::wl_state::PlatformSurface, &frame)
    }

    fn surface_present_software(
        &self,
        s: SurfaceHandle,
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

    fn surface_resize(
        &self,
        s: SurfaceHandle,
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

    fn surface_set_visible(&self, s: SurfaceHandle, visible: bool) {
        wl_ops::surface_set_visible(
            s as *mut crate::wl_state::PlatformSurface,
            visible,
            BG_R,
            BG_G,
            BG_B,
        );
    }

    fn restack(&self, ordered: *const SurfaceHandle, n: usize) {
        if ordered.is_null() {
            return;
        }
        let typed: &[*mut crate::wl_state::PlatformSurface] = unsafe {
            std::slice::from_raw_parts(
                ordered as *const *mut crate::wl_state::PlatformSurface,
                n,
            )
        };
        wl_ops::restack(typed);
    }

    fn fade_surface(
        &self,
        s: SurfaceHandle,
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
            // No wp_alpha_modifier_v1 (e.g. niri) or no surface/fps:
            // hard-unmap and fire the callback contract inline.
            if !s.is_null() {
                wl_ops::surface_set_visible(surf_ptr, false, BG_R, BG_G, BG_B);
            }
            unsafe {
                if let Some(f) = on_start { f(start_ctx) }
                if let Some(d) = start_dtor { d(start_ctx) }
                if let Some(f) = on_done { f(done_ctx) }
                if let Some(d) = done_dtor { d(done_ctx) }
            }
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

    fn popup_show(&self, s: SurfaceHandle, req: *const JfnPopupRequest) {
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

    fn popup_hide(&self, s: SurfaceHandle) {
        wl_ops::popup_hide(s as *mut crate::wl_state::PlatformSurface);
    }

    fn popup_present(
        &self,
        s: SurfaceHandle,
        info: *const c_void,
        lw: c_int,
        lh: c_int,
    ) {
        let Some(frame) = (unsafe { to_dmabuf_frame(info) }) else {
            return;
        };
        wl_ops::popup_present(
            s as *mut crate::wl_state::PlatformSurface,
            &frame,
            lw,
            lh,
        );
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

    fn set_fullscreen(&self, v: bool) {
        unsafe { crate::wl_ffi::jfn_wl_set_fullscreen(v) };
    }

    fn toggle_fullscreen(&self) {
        unsafe { crate::wl_ffi::jfn_wl_toggle_fullscreen() };
    }

    fn begin_transition(&self) {
        unsafe { crate::wl_ffi::jfn_wl_begin_transition() };
    }

    fn end_transition(&self) {
        unsafe { crate::wl_ffi::jfn_wl_end_transition() };
    }

    fn in_transition(&self) -> bool {
        unsafe { crate::wl_ffi::jfn_wl_in_transition() }
    }

    fn get_scale(&self) -> f32 {
        unsafe { jfn_wl_get_cached_scale() }
    }

    fn get_display_scale(&self, x: c_int, y: c_int) -> f32 {
        let s = unsafe { jfn_wayland_scale_probe(x, y) };
        if s > 0.0 { s as f32 } else { 1.0 }
    }

    fn set_cursor(&self, ty: c_int) {
        crate::input_lifecycle::set_cursor_active(ty as u32);
    }

    fn set_idle_inhibit(&self, level: IdleInhibitLevel) {
        jfn_idle_inhibit_linux::set(level as u32);
    }

    fn set_theme_color(&self, _rgb: u32) {
        #[cfg(feature = "kde-palette")]
        {
            let r = ((_rgb >> 16) & 0xFF) as u8;
            let g = ((_rgb >> 8) & 0xFF) as u8;
            let b = (_rgb & 0xFF) as u8;
            // hex string "#RRGGBB\0".
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
            unsafe {
                jfn_wl_kde_palette_set_color(r, g, b, hex.as_ptr() as *const c_char);
            }
        }
    }

    fn shared_texture_supported(&self) -> bool {
        self.shared_texture.load(Ordering::Acquire)
    }

    fn set_shared_texture_unsupported(&self) {
        self.shared_texture.store(false, Ordering::Release);
    }

    fn clipboard_text_supported(&self) -> bool {
        self.clipboard.load(Ordering::Acquire)
    }

    fn clear_clipboard_handler(&self) {
        self.clipboard.store(false, Ordering::Release);
    }

    fn clipboard_read_text_async(
        &self,
        on_done: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
        ctx: *mut c_void,
        dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    ) {
        if !self.clipboard.load(Ordering::Acquire) {
            unsafe {
                if let Some(cb) = on_done {
                    cb(ctx, c"".as_ptr(), 0);
                }
                if let Some(d) = dtor {
                    d(ctx);
                }
            }
            return;
        }
        crate::clipboard::clipboard_read_text_async(on_done, ctx, dtor);
    }

    fn open_external_url(&self, utf8: *const c_char, _len: usize) {
        if !utf8.is_null() {
            unsafe { jfn_open_url(utf8) };
        }
    }
}

/// Build a boxed Wayland platform. Called from jfn_app_main on Linux when
/// the selected backend is Wayland.
pub fn make_wayland_platform() -> Box<dyn Platform> {
    Box::new(WaylandPlatform::new())
}
