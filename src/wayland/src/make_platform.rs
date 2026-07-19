//! Wayland backend impl of [`jfn_platform_abi::Platform`].
//!
//! This is the crate's ABI adapter: raw pointers, `c_int` dimensions, and
//! opaque `SurfaceHandle`s from the Platform trait are converted here into
//! the crate's domain types before reaching any internal module. The factory
//! returns the concrete type; `jfn_app_main` boxes it as `Box<dyn Platform>`
//! before handing it to `jfn_platform_abi::install`.

#![allow(non_snake_case)]
// Platform trait carries raw-pointer args (dmabuf info, accel-paint info)
// from CEF; trait impls forward them unchanged to unsafe FFI fns.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

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
    let id = {
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(dup_fd, &mut st) } == 0 {
            Some((st.st_dev as u64, st.st_ino as u64))
        } else {
            None
        }
    };
    let fd = unsafe { std::os::fd::OwnedFd::from_raw_fd(dup_fd) };
    Some(JfnDmabufFrame {
        fd,
        id,
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

    /// A preference among the available options only — availability itself is
    /// protocol-derived in [`Self::window_decoration_options`].
    fn default_window_decorations(&self) -> WindowDecorations {
        jfn_linux_util::default_window_decorations()
    }

    fn window_decoration_options(&self) -> jfn_platform_abi::DecorationOptions {
        let globals = crate::decoration_probe::globals();
        jfn_platform_abi::DecorationOptions::with_server(
            cfg!(feature = "kde-palette") && globals.kde_palette,
        )
    }

    fn early_init(&self) {
        crate::decoration_probe::init();
    }

    fn init(&self, _mpv: *mut c_void) -> bool {
        crate::lifecycle::init()
    }

    fn cleanup(&self) {
        crate::lifecycle::cleanup();
    }

    fn post_window_cleanup(&self) {
        crate::mpv_host::stop_proxy();
        #[cfg(feature = "kde-palette")]
        crate::kde_palette::post_window_cleanup();
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
        &crate::mpv_host::WaylandMpvHost
    }

    fn media_session(&self) -> &dyn jfn_platform_abi::MediaSink {
        &jfn_mpris::MprisSink
    }

    fn cef_paths(&self) -> jfn_platform_abi::CefPaths {
        jfn_linux_util::cef_paths()
    }

    fn window_source(&self) -> &'static dyn jfn_platform_abi::WindowSource {
        &crate::window_source::WaylandWindowSource
    }

    // Wayland owns its toplevel and sizes it in apply_boot_geometry, so mpv
    // neither sizes at boot nor reconciles on scale change.
    fn boot_mpv_geometry(&self, _g: &BootGeometry) -> Option<String> {
        None
    }

    fn reconcile_mpv_size(
        &self,
        _display_hidpi_scale: f64,
        _saved_scale: f32,
        _saved_logical: jfn_platform_abi::LogicalSize,
        _locked: bool,
    ) -> Option<jfn_platform_abi::PhysicalSize> {
        None
    }

    fn set_fullscreen(&self, v: bool) {
        crate::root_window::set_fullscreen(v);
    }

    fn toggle_fullscreen(&self) {
        crate::root_window::toggle_fullscreen();
    }

    fn window_minimize(&self) {
        crate::root_window::set_minimized();
    }

    fn window_toggle_maximize(&self) {
        crate::root_window::toggle_maximize();
    }

    fn window_start_move(&self) {
        crate::root_window::start_move();
    }

    fn window_start_resize(&self, edge: c_int) {
        crate::root_window::start_resize(edge as u32);
    }

    fn get_scale(&self) -> f32 {
        crate::window_state::cached_scale()
    }

    fn effective_scale(&self, _mpv_display_hidpi_scale: f64) -> f32 {
        self.get_scale()
    }

    fn get_display_scale(&self, x: c_int, y: c_int) -> f32 {
        crate::scale_probe::probe_scale(crate::scale_probe::ProbeTarget::Point { x, y })
            .map_or(1.0, |s| s.ratio_f32())
    }

    fn apply_boot_geometry(&self, g: &BootGeometry) {
        // Only the host's own window geometry uses the boot size; mpv mirrors the
        // committed window geometry and never the boot guess.
        crate::root_window::set_boot_geometry(g.logical().w, g.logical().h, g.maximized());
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

        crate::root_window::set_background_color(r, g, b);

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
            if let Ok(hex) = std::ffi::CStr::from_bytes_with_nul(&hex) {
                crate::kde_palette::set_color(r, g, b, hex);
            }
        }
    }

    fn window_decorations_supported(&self) -> bool {
        self.window_decoration_options().has_choice()
    }

    fn effective_decorations(&self) -> jfn_platform_abi::EffectiveDecorations {
        crate::root_window::effective_decorations()
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
