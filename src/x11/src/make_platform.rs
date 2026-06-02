//! X11 backend impl of [`jfn_platform_abi::Platform`].

#![allow(non_snake_case)]
// Platform trait carries raw-pointer args (dirty rects, accel-paint info)
// from CEF; trait impls forward them unchanged to unsafe FFI fns.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::ffi::{c_int, c_void};

use crate::surface::{
    jfn_x11_alloc_surface, jfn_x11_free_surface, jfn_x11_restack, jfn_x11_surface_present,
    jfn_x11_surface_present_software, jfn_x11_surface_resize, jfn_x11_surface_set_visible,
};

pub use jfn_platform_abi::{
    DisplayBackend, IdleInhibitLevel, JfnPopupRequest, JfnRect, Platform, SurfaceHandle,
    SurfaceSize, WindowDecorations, WindowGeometry, WindowPos,
};

use jfn_mpv::api::{jfn_mpv_set_fullscreen, jfn_mpv_toggle_fullscreen};
use jfn_mpv::boot::jfn_mpv_handle_get;
use jfn_playback::ingest_driver::{jfn_playback_display_scale, jfn_playback_fullscreen};

pub struct X11Platform;

impl Platform for X11Platform {
    fn display(&self) -> DisplayBackend {
        DisplayBackend::X11
    }

    fn default_window_decorations(&self) -> WindowDecorations {
        jfn_linux_util::default_window_decorations()
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
        dirty: &[JfnRect],
        buffer: *const c_void,
        w: c_int,
        h: c_int,
    ) -> bool {
        unsafe {
            jfn_x11_surface_present_software(
                s as *mut crate::x11_state::PlatformSurface,
                dirty.as_ptr(),
                dirty.len(),
                buffer,
                w,
                h,
            )
        }
    }

    fn surface_resize(&self, s: SurfaceHandle, size: SurfaceSize) {
        unsafe {
            jfn_x11_surface_resize(
                s as *mut crate::x11_state::PlatformSurface,
                size.logical_w,
                size.logical_h,
                size.physical_w,
                size.physical_h,
            )
        };
    }

    fn surface_set_visible(&self, s: SurfaceHandle, visible: bool) {
        unsafe {
            jfn_x11_surface_set_visible(s as *mut crate::x11_state::PlatformSurface, visible)
        };
    }

    fn restack(&self, handles: &[SurfaceHandle]) {
        unsafe {
            jfn_x11_restack(
                handles.as_ptr() as *const *mut crate::x11_state::PlatformSurface,
                handles.len(),
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
            if let Some(m) = crate::x11_state::MUT.lock().as_mut() {
                m.cached_scale = f;
            }
            return f;
        }
        if let Some(m) = crate::x11_state::MUT.lock().as_ref()
            && m.cached_scale > 0.0
        {
            return m.cached_scale;
        }
        1.0
    }

    fn query_window_position(&self) -> Option<WindowPos> {
        let conn = crate::x11_state::conn()?;
        let g = crate::x11_state::MUT.lock();
        let m = g.as_ref()?;
        let (x, y, _, _) = crate::lifecycle::query_parent_geometry(&conn, m.parent, m.root)?;
        Some(WindowPos { x, y })
    }

    fn clamp_window_geometry(&self, g: WindowGeometry) -> WindowGeometry {
        // X11 constrains only the size; position is left to the WM.
        let (mut w, mut h) = (g.w, g.h);
        crate::lifecycle::clamp_window_geometry(&mut w, &mut h);
        WindowGeometry {
            w,
            h,
            x: g.x,
            y: g.y,
        }
    }

    fn set_cursor(&self, t: c_int) {
        crate::input_lifecycle::set_cursor_active(t as u32);
    }

    fn set_idle_inhibit(&self, level: IdleInhibitLevel) {
        jfn_linux_util::idle_inhibit::set(level as u32);
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
        jfn_linux_util::open_url::open(url);
    }
}

pub fn make_x11_platform() -> Box<dyn Platform> {
    Box::new(X11Platform)
}
