//! Wayland backend impl of [`jfn_platform_abi::Platform`].
//!
//! Each method forwards to the existing Rust `wl_*` / `jfn_wl_*` helpers
//! (mostly `crate::wl_ops` + `crate::wl_ffi`). The factory returns the
//! concrete type; `jfn_app_main` boxes it as `Box<dyn Platform>` before
//! handing it to `jfn_platform_abi::install`.

#![allow(non_snake_case)]
// Platform trait carries raw-pointer args (dmabuf info, accel-paint info)
// from CEF; trait impls forward them unchanged to unsafe FFI fns.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

#[cfg(feature = "kde-palette")]
use std::ffi::c_char;
use std::ffi::{c_int, c_void};
use std::os::fd::FromRawFd;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::wl_ops::{self, JfnDmabufFrame};

use jfn_platform_abi::cursor::CursorShape;
pub use jfn_platform_abi::{
    BootGeometry, DisplayBackend, IdleInhibitLevel, JfnContextMenuRequest, JfnPopupRequest,
    JfnRect, Platform, SurfaceHandle, SurfaceSize, WindowDecorations,
};

// =====================================================================
// External symbols
// =====================================================================

#[cfg(feature = "kde-palette")]
use crate::kde_palette::{jfn_wl_kde_palette_post_window_cleanup, jfn_wl_kde_palette_set_color};
use crate::lifecycle::{jfn_wl_lifecycle_cleanup, jfn_wl_lifecycle_init};
use crate::proxy::jfn_wl_get_cached_scale;
use crate::scale_probe::jfn_wayland_scale_probe;

// =====================================================================
// Helpers
// =====================================================================

// Background color matches kBgColor (0x101010). Hard-coded here so the
// surface_set_visible path doesn't need to carry the color.
const BG_R: u8 = 0x10;
const BG_G: u8 = 0x10;
const BG_B: u8 = 0x10;

pub(crate) unsafe fn to_dmabuf_frame(info: *const c_void) -> Option<JfnDmabufFrame> {
    let info = info as *const cef::sys::_cef_accelerated_paint_info_t;
    if info.is_null() {
        return None;
    }
    let info = unsafe { &*info };
    let plane0 = &info.planes[0];
    let dup_fd = unsafe { libc::dup(plane0.fd) };
    if dup_fd < 0 {
        return None;
    }
    // OwnedFd closes the dup on drop once the frame is presented.
    let fd = unsafe { std::os::fd::OwnedFd::from_raw_fd(dup_fd) };
    Some(JfnDmabufFrame {
        fd,
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

    fn default_window_decorations(&self) -> WindowDecorations {
        jfn_linux_util::default_window_decorations()
    }

    fn init(&self, _mpv: *mut c_void) -> bool {
        jfn_wl_lifecycle_init()
    }

    fn cleanup(&self) {
        jfn_wl_lifecycle_cleanup();
    }

    fn post_window_cleanup(&self) {
        crate::wlproxy_host::stop_wlproxy();
        #[cfg(feature = "kde-palette")]
        jfn_wl_kde_palette_post_window_cleanup();
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
        dirty: &[JfnRect],
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
            dirty,
            pixels,
            w,
            h,
        )
    }

    fn surface_resize(&self, s: SurfaceHandle, size: SurfaceSize) {
        wl_ops::surface_resize(
            s as *mut crate::wl_state::PlatformSurface,
            size.logical_w,
            size.logical_h,
            size.physical_w,
            size.physical_h,
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

    fn restack(&self, ordered: &[SurfaceHandle]) {
        // SAFETY: a `&[SurfaceHandle]` (i.e. `&[*mut c_void]`) and a
        // `&[*mut PlatformSurface]` have identical layout; each handle was
        // minted by this backend's `alloc_surface`.
        let typed: &[*mut crate::wl_state::PlatformSurface] = unsafe {
            std::slice::from_raw_parts(
                ordered.as_ptr() as *const *mut crate::wl_state::PlatformSurface,
                ordered.len(),
            )
        };
        wl_ops::restack(typed);
    }

    fn dropdown_backend(&self) -> &'static dyn jfn_platform_abi::DropdownBackend {
        crate::dropdown::backend()
    }

    fn context_menu_backend(&self) -> &'static dyn jfn_platform_abi::ContextMenuBackend {
        crate::context_menu::backend()
    }

    fn mpv_host(&self) -> &dyn jfn_platform_abi::MpvHost {
        &crate::wlproxy_host::WlproxyMpvHost
    }

    fn media_session(&self) -> &dyn jfn_platform_abi::MediaSink {
        &jfn_mpris::MprisSink
    }

    fn window_source(&self) -> Option<&'static dyn jfn_platform_abi::WindowSource> {
        Some(&crate::window_source::WaylandWindowSource)
    }

    fn set_fullscreen(&self, v: bool) {
        crate::wl_ffi::jfn_wl_set_fullscreen(v);
    }

    fn toggle_fullscreen(&self) {
        crate::wl_ffi::jfn_wl_toggle_fullscreen();
    }

    fn window_minimize(&self) {
        crate::wl_ffi::jfn_wl_window_minimize();
    }

    fn window_toggle_maximize(&self) {
        crate::wl_ffi::jfn_wl_window_toggle_maximize();
    }

    fn window_start_move(&self) {
        crate::wl_ffi::jfn_wl_window_start_move();
    }

    fn window_start_resize(&self, edge: c_int) {
        crate::wl_ffi::jfn_wl_window_start_resize(edge);
    }

    fn begin_transition(&self) {
        crate::wl_ffi::jfn_wl_begin_transition();
    }

    fn end_transition(&self) {
        crate::wl_ffi::jfn_wl_end_transition();
    }

    fn in_transition(&self) -> bool {
        crate::wl_ffi::jfn_wl_in_transition()
    }

    fn get_scale(&self) -> f32 {
        jfn_wl_get_cached_scale()
    }

    fn effective_scale(&self, _mpv_display_hidpi_scale: f64) -> f32 {
        self.get_scale()
    }

    fn get_display_scale(&self, x: c_int, y: c_int) -> f32 {
        let s = jfn_wayland_scale_probe(x, y);
        if s > 0.0 { s as f32 } else { 1.0 }
    }

    fn apply_boot_geometry(&self, g: &BootGeometry) {
        jfn_wlproxy::jfn_wlproxy_set_initial_size(g.logical.w, g.logical.h);
        jfn_wlproxy::jfn_wlproxy_set_initial_maximized(g.maximized);
    }

    fn set_cursor(&self, shape: CursorShape) {
        crate::input_lifecycle::set_cursor_active(shape);
    }

    fn set_idle_inhibit(&self, level: IdleInhibitLevel) {
        jfn_linux_util::idle_inhibit::set(level as u32);
    }

    fn set_theme_color(&self, rgb: u32) {
        let r = ((rgb >> 16) & 0xFF) as u8;
        let g = ((rgb >> 8) & 0xFF) as u8;
        let b = (rgb & 0xFF) as u8;

        jfn_wlproxy::jfn_wlproxy_set_background_color(r, g, b);

        #[cfg(feature = "kde-palette")]
        {
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

    fn window_decorations_supported(&self) -> bool {
        true
    }

    fn theme_color_supported(&self) -> bool {
        cfg!(feature = "kde-palette")
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

    fn clipboard_read_text_async(&self, on_done: Box<dyn FnOnce(&str) + Send>) {
        if !self.clipboard.load(Ordering::Acquire) {
            on_done("");
            return;
        }
        crate::clipboard::clipboard_read_text_async(on_done);
    }

    fn open_external_url(&self, url: &str) {
        jfn_linux_util::open_url::open(url);
    }

    fn open_path(&self, path: &std::path::Path) {
        jfn_linux_util::open_url::open(&path.to_string_lossy());
    }
}

/// Build a boxed Wayland platform. Called from jfn_app_main on Linux when
/// the selected backend is Wayland.
pub fn make_wayland_platform() -> Box<dyn Platform> {
    Box::new(WaylandPlatform::new())
}
