//! `Platform` trait + global handle held by `jfn_app_main`.
//!
//! Each backend crate (`jfn-wayland`, `jfn-x11`, `jfn-macos`, `jfn-windows`)
//! returns a concrete type implementing this trait via its
//! `make_*_platform()` factory. The binary installs the chosen backend into
//! the [`OnceLock`] below via [`install`] / [`get`].
//!
//! The previous C-ABI `Platform` struct (a vtable of `Option<extern "C"
//! fn(...)>`) is gone — backends now author native Rust methods on a
//! concrete impl rather than populating a fn-pointer table. The shared
//! types (`JfnRect`, `JfnPopupRequest`, `DisplayBackend`) stay `#[repr(C)]`
//! so they can still cross the CEF C ABI surface (`OnAcceleratedPaint`
//! accel-paint info etc).

#![allow(non_snake_case)]

use std::ffi::{c_char, c_int, c_void, CString};
use std::sync::OnceLock;
use std::sync::atomic::{AtomicPtr, Ordering};

#[repr(i32)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum DisplayBackend {
    Wayland = 0,
    X11 = 1,
    Windows = 2,
    MacOS = 3,
}

#[repr(C)]
pub struct JfnRect {
    pub x: c_int,
    pub y: c_int,
    pub w: c_int,
    pub h: c_int,
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

/// Idle-inhibit level. Mirrors C++ `IdleInhibitLevel`.
#[repr(i32)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum IdleInhibitLevel {
    None = 0,
    System = 1,
    Display = 2,
}

/// Backend-allocated per-surface handle. Backends define the layout
/// in-crate; callers only ever hold the raw pointer.
pub type SurfaceHandle = *mut c_void;

/// Process-wide platform handle. Each method maps 1:1 to the previous
/// vtable slot; `Option`-able vtable slots become default no-op methods so
/// backends only override what they care about.
///
/// All methods take `&self` — backends keep their own interior mutability
/// (`Mutex`, `AtomicBool`, etc) where they need it.
pub trait Platform: Send + Sync {
    fn display(&self) -> DisplayBackend;

    fn early_init(&self) {}
    fn init(&self, mpv: *mut c_void) -> bool {
        let _ = mpv;
        true
    }
    fn cleanup(&self) {}
    fn post_window_cleanup(&self) {}

    // Per-surface
    fn alloc_surface(&self) -> SurfaceHandle {
        std::ptr::null_mut()
    }
    fn free_surface(&self, _s: SurfaceHandle) {}
    fn surface_present(&self, _s: SurfaceHandle, _info: *const c_void) -> bool {
        false
    }
    fn surface_present_software(
        &self,
        _s: SurfaceHandle,
        _dirty: *const JfnRect,
        _dirty_len: usize,
        _buffer: *const c_void,
        _w: c_int,
        _h: c_int,
    ) -> bool {
        false
    }
    fn surface_resize(
        &self,
        _s: SurfaceHandle,
        _lw: c_int,
        _lh: c_int,
        _pw: c_int,
        _ph: c_int,
    ) {
    }
    fn surface_set_visible(&self, _s: SurfaceHandle, _visible: bool) {}
    fn restack(&self, _ordered: *const SurfaceHandle, _n: usize) {}
    #[allow(clippy::too_many_arguments)]
    fn fade_surface(
        &self,
        _s: SurfaceHandle,
        _sec: f32,
        _on_start: Option<unsafe extern "C" fn(*mut c_void)>,
        _start_ctx: *mut c_void,
        _start_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
        _on_done: Option<unsafe extern "C" fn(*mut c_void)>,
        _done_ctx: *mut c_void,
        _done_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    ) {
    }

    // Popup
    fn popup_show(&self, _s: SurfaceHandle, _req: *const JfnPopupRequest) {}
    fn popup_hide(&self, _s: SurfaceHandle) {}
    fn popup_present(
        &self,
        _s: SurfaceHandle,
        _info: *const c_void,
        _lw: c_int,
        _lh: c_int,
    ) {
    }
    fn popup_present_software(
        &self,
        _s: SurfaceHandle,
        _buffer: *const c_void,
        _pw: c_int,
        _ph: c_int,
        _lw: c_int,
        _lh: c_int,
    ) {
    }

    // Fullscreen
    fn set_fullscreen(&self, _v: bool) {}
    fn toggle_fullscreen(&self) {}

    // Transition
    fn begin_transition(&self) {}
    fn end_transition(&self) {}
    fn in_transition(&self) -> bool {
        false
    }
    fn set_expected_size(&self, _w: c_int, _h: c_int) {}

    fn get_scale(&self) -> f32 {
        1.0
    }
    fn get_display_scale(&self, _x: c_int, _y: c_int) -> f32 {
        1.0
    }

    fn query_window_position(&self, _x: *mut c_int, _y: *mut c_int) -> bool {
        false
    }
    fn clamp_window_geometry(
        &self,
        _w: *mut c_int,
        _h: *mut c_int,
        _x: *mut c_int,
        _y: *mut c_int,
    ) {
    }

    fn pump(&self) {}
    fn run_main_loop(&self) {}
    fn wake_main_loop(&self) {}

    fn set_cursor(&self, _cef_cursor_type: c_int) {}
    fn set_idle_inhibit(&self, _level: IdleInhibitLevel) {}
    fn set_theme_color(&self, _rgb: u32) {}

    fn shared_texture_supported(&self) -> bool {
        true
    }
    /// Set during init by Wayland backend (dmabuf probe) when GPU lacks the
    /// shared-texture path.
    fn set_shared_texture_unsupported(&self) {}

    /// CEF ozone platform name (e.g. "wayland" / "x11"). Stored in the
    /// shared `OZONE_PLATFORM` cell below — the default impls cover every
    /// backend; no backend needs to override these.
    fn cef_ozone_platform(&self) -> *const c_char {
        ozone_platform_get()
    }
    fn set_cef_ozone_platform(&self, utf8: *const c_char) {
        ozone_platform_set(utf8);
    }

    /// Whether [`clipboard_read_text_async`] will actually invoke the
    /// backend clipboard. Wayland clears this in `wl_init` when no data
    /// device manager is present; the menu Paste path uses it to decide
    /// between native OS read vs CEF `frame.Paste()`.
    fn clipboard_text_supported(&self) -> bool {
        true
    }

    fn clipboard_read_text_async(
        &self,
        on_done: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
        ctx: *mut c_void,
        dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    ) {
        // No backend support — fire empty callback and dtor synchronously,
        // matching the old C++ fallback in `platform_ops.cpp`.
        unsafe {
            if let Some(cb) = on_done {
                cb(ctx, c"".as_ptr(), 0);
            }
            if let Some(d) = dtor {
                d(ctx);
            }
        }
    }
    /// Disable subsequent clipboard reads (set by Wayland when no data
    /// device manager is available).
    fn clear_clipboard_handler(&self) {}

    fn open_external_url(&self, _url: &str) {}
}

// =====================================================================
// Process-wide handle
// =====================================================================

// `OnceLock<Box<dyn Platform>>` doesn't give us a stable `'static` reference
// shape that's ergonomic for the existing `unsafe extern "C"` thunks below;
// store a raw fat pointer instead. Set exactly once during boot.
static PLATFORM: OnceLock<&'static dyn Platform> = OnceLock::new();

/// Install the platform backend. Must be called exactly once during boot,
/// before any other code dispatches through [`get`]. Panics if called
/// twice — there is no "swap backend at runtime" path.
pub fn install(p: Box<dyn Platform>) {
    let leaked: &'static dyn Platform = Box::leak(p);
    PLATFORM
        .set(leaked)
        .map_err(|_| ())
        .expect("install() called twice");
}

// CEF ozone platform name (NUL-terminated). Set once by jfn_app_main
// before `Platform::init`; read by the Wayland dmabuf probe and the C++
// side via `jfn_platform_cef_ozone_platform`. Kept in `Mutex` for the
// (very rare) set, but stable for read.
static OZONE_PLATFORM: AtomicPtr<c_char> = AtomicPtr::new(std::ptr::null_mut());

fn ozone_platform_get() -> *const c_char {
    let p = OZONE_PLATFORM.load(Ordering::Acquire);
    if p.is_null() { c"".as_ptr() } else { p as *const c_char }
}

fn ozone_platform_set(utf8: *const c_char) {
    let cs = if utf8.is_null() {
        CString::default()
    } else {
        unsafe { std::ffi::CStr::from_ptr(utf8).to_owned() }
    };
    // Leak: process-static string, one assignment per boot. Previous value
    // (if any) is leaked too; cheap on a 32-byte string.
    let leaked = cs.into_raw();
    OZONE_PLATFORM.store(leaked, Ordering::Release);
}

/// Returns the installed platform backend. Panics if [`install`] hasn't
/// been called yet — every call site is post-boot.
pub fn get() -> &'static dyn Platform {
    *PLATFORM
        .get()
        .expect("jfn_platform_abi::get() called before install()")
}

/// Like [`get`] but returns `None` before install. Used by jfn_cef's
/// `OnConsoleMessage` and similar paths that may fire during early CEF
/// helper-process boot when no platform is installed.
pub fn try_get() -> Option<&'static dyn Platform> {
    PLATFORM.get().copied()
}

