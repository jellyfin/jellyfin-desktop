//! X11 backend impl of [`jfn_platform_abi::Platform`].

#![allow(non_snake_case)]

use std::ffi::{c_int, c_void};

use crate::surface::{
    jfn_x11_alloc_surface, jfn_x11_fade_surface, jfn_x11_free_surface, jfn_x11_restack,
    jfn_x11_surface_present, jfn_x11_surface_present_software, jfn_x11_surface_resize,
    jfn_x11_surface_set_visible,
};

pub use jfn_platform_abi::{
    DisplayBackend, IdleInhibitLevel, JfnPopupRequest, JfnRect, Platform, SurfaceHandle,
};

use jfn_mpv::api::{jfn_mpv_set_fullscreen, jfn_mpv_toggle_fullscreen};
use jfn_mpv::boot::jfn_mpv_handle_get;
use jfn_playback::ingest_driver::{jfn_playback_display_scale, jfn_playback_fullscreen};

pub struct X11Platform;

impl Platform for X11Platform {
    fn display(&self) -> DisplayBackend {
        DisplayBackend::X11
    }

    fn init(&self, _mpv: *mut c_void) -> bool {
        crate::lifecycle::init()
    }

    fn cleanup(&self) {
        crate::lifecycle::cleanup();
    }

    fn alloc_surface(&self) -> SurfaceHandle {
        jfn_x11_alloc_surface() as *mut c_void
    }

    fn free_surface(&self, s: SurfaceHandle) {
        unsafe { jfn_x11_free_surface(s as *mut crate::x11_state::PlatformSurface) };
    }

    fn surface_present(&self, s: SurfaceHandle, info: *const c_void) -> bool {
        jfn_x11_surface_present(s as *mut crate::x11_state::PlatformSurface, info)
    }

    fn surface_present_software(
        &self,
        s: SurfaceHandle,
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

    fn surface_resize(&self, s: SurfaceHandle, lw: c_int, lh: c_int, pw: c_int, ph: c_int) {
        unsafe {
            jfn_x11_surface_resize(s as *mut crate::x11_state::PlatformSurface, lw, lh, pw, ph)
        };
    }

    fn surface_set_visible(&self, s: SurfaceHandle, visible: bool) {
        unsafe {
            jfn_x11_surface_set_visible(s as *mut crate::x11_state::PlatformSurface, visible)
        };
    }

    fn restack(&self, handles: *const SurfaceHandle, n: usize) {
        unsafe { jfn_x11_restack(handles as *const *mut crate::x11_state::PlatformSurface, n) };
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
            )
        };
    }

    fn popup_show(&self, _s: SurfaceHandle, _req: JfnPopupRequest) {
        // CEF dispatches selection itself on X11; drop the closure.
    }

    fn set_fullscreen(&self, fullscreen: bool) {
        if jfn_mpv_handle_get().is_null() {
            return;
        }
        if jfn_playback_fullscreen() == fullscreen {
            return;
        }
        jfn_mpv_set_fullscreen(fullscreen);
    }

    fn toggle_fullscreen(&self) {
        if !jfn_mpv_handle_get().is_null() {
            jfn_mpv_toggle_fullscreen();
        }
    }

    fn get_scale(&self) -> f32 {
        let s = jfn_playback_display_scale();
        if s > 0.0 {
            let f = s as f32;
            if let Ok(mut g) = crate::x11_state::MUT.lock()
                && let Some(m) = g.as_mut()
            {
                m.cached_scale = f;
            }
            return f;
        }
        if let Ok(g) = crate::x11_state::MUT.lock()
            && let Some(m) = g.as_ref()
            && m.cached_scale > 0.0
        {
            return m.cached_scale;
        }
        1.0
    }

    fn query_window_position(&self, x: *mut c_int, y: *mut c_int) -> bool {
        let Some(conn) = crate::x11_state::conn() else {
            return false;
        };
        let g = crate::x11_state::MUT.lock().unwrap();
        let Some(m) = g.as_ref() else { return false };
        let Some((px, py, _, _)) = crate::lifecycle::query_parent_geometry(&conn, m.parent, m.root)
        else {
            return false;
        };
        unsafe {
            *x = px;
            *y = py;
        }
        true
    }

    fn clamp_window_geometry(&self, w: *mut c_int, h: *mut c_int, _x: *mut c_int, _y: *mut c_int) {
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

    fn set_cursor(&self, t: c_int) {
        crate::input_lifecycle::set_cursor_active(t as u32);
    }

    fn set_idle_inhibit(&self, level: IdleInhibitLevel) {
        jfn_idle_inhibit_linux::set(level as u32);
    }

    fn shared_texture_supported(&self) -> bool {
        false
    }

    fn clipboard_text_supported(&self) -> bool {
        false
    }

    fn clipboard_read_text_async(&self, on_done: Box<dyn FnOnce(&str) + Send>) {
        // X11 has no native clipboard read path here — fire empty result.
        on_done("");
    }

    fn open_external_url(&self, url: &str) {
        jfn_open_url_linux::open(url);
    }
}

pub fn make_x11_platform() -> Box<dyn Platform> {
    Box::new(X11Platform)
}
