//! Wayland-backend `Platform::init` / `Platform::cleanup` body.
//!
//! Drives the per-process Wayland subsystems in the order required by the
//! original `src/platform/wayland.cpp::wl_init`: read mpv's wayland-display
//! and -surface handles, prime the cached fullscreen, wire input, bring up
//! the core state, install mpv's close-cb trampoline, init EGL, probe
//! dmabuf support, attach the KDE palette manager, start the input thread,
//! and bring up the clipboard reader.
//!
//! The C++ side keeps the `Platform` vtable construction — only the
//! orchestration body moves here.

use std::ffi::{CStr, c_char, c_void};

use crate::egl_dyn as egl;

// =====================================================================
// FFI declarations consumed during init/cleanup.
// =====================================================================

unsafe extern "C" {
    // mpv property reads
    fn jfn_mpv_get_property_int(name: *const c_char, out: *mut i64) -> i32;
    // Shutdown trampoline target for the wayland-close-cb-ptr property.
    fn jfn_shutdown_initiate();

    // Platform field accessors (defined in src/platform/platform_ops.cpp).
    fn jfn_platform_cef_ozone_platform() -> *const c_char;
    fn jfn_platform_set_shared_texture_unsupported();
    fn jfn_platform_clear_clipboard_handler();

    // Wayland-side subsystems already living in this crate's FFI surface.
    fn jfn_wl_dmabuf_probe(ozone_platform: *const c_char, egl_dpy: *mut c_void) -> bool;
}

// =====================================================================
// Helpers
// =====================================================================

fn mpv_prop_intptr(name: &CStr) -> Option<usize> {
    let mut v: i64 = 0;
    let rc = unsafe { jfn_mpv_get_property_int(name.as_ptr(), &mut v) };
    if rc == 0 && v != 0 { Some(v as usize) } else { None }
}

// Installs `cb` into mpv's wayland-close-cb-ptr slot (a
// `void(**)(void*)` followed by a `void**` data slot, packed by libmpv).
// Passing `None` clears the slot.
unsafe fn write_close_cb(slot: usize, cb: Option<unsafe extern "C" fn(*mut c_void)>) {
    let fn_slot = slot as *mut Option<unsafe extern "C" fn(*mut c_void)>;
    let data_slot = (slot + std::mem::size_of::<usize>()) as *mut *mut c_void;
    unsafe {
        *fn_slot = cb;
        if cb.is_some() {
            *data_slot = std::ptr::null_mut();
        }
    }
}

unsafe extern "C" fn close_cb_trampoline(_: *mut c_void) {
    unsafe { jfn_shutdown_initiate() };
}

// =====================================================================
// init / cleanup
// =====================================================================

#[unsafe(no_mangle)]
pub extern "C" fn jfn_wl_lifecycle_init() -> bool {
    let display = match mpv_prop_intptr(c"wayland-display") {
        Some(p) => p as *mut c_void,
        None => {
            log::error!("Failed to get Wayland display from mpv");
            return false;
        }
    };
    let parent = match mpv_prop_intptr(c"wayland-surface") {
        Some(p) => p as *mut c_void,
        None => {
            log::error!("Failed to get Wayland surface from mpv");
            return false;
        }
    };

    // Seed Rust state with mpv's current fullscreen — first configure
    // after this point won't start a spurious transition.
    crate::wl_ffi::jfn_wl_core_set_was_fullscreen(unsafe { jfn_playback_fullscreen() });

    // Prepare the input layer first so its xkb context is ready before
    // any seat_caps wires up keyboard listeners that need xkb.
    crate::input_lifecycle::lifecycle_init(display);

    if !unsafe { crate::wl_ffi::jfn_wl_core_init(display, parent) } {
        log::error!("jfn_wl_core_init failed");
        return false;
    }

    // Register close callback — intercepts xdg_toplevel close before mpv
    // sees it. mpv exposes the slot as a `(fn-ptr, data-ptr)` pair packed
    // at the address it returns through `wayland-close-cb-ptr`.
    if let Some(slot) = mpv_prop_intptr(c"wayland-close-cb-ptr") {
        unsafe { write_close_cb(slot, Some(close_cb_trampoline)) };
    }

    // EGL init for CEF shared texture support + dmabuf probe.
    let egl_dpy: *mut c_void = match egl::Egl::load_default() {
        Ok(api) => unsafe {
            let d = (api.get_display)(display as egl::NativeDisplayType);
            if d.is_null() {
                std::ptr::null_mut()
            } else {
                let mut major: egl::Int = 0;
                let mut minor: egl::Int = 0;
                (api.initialize)(d, &mut major, &mut minor);
                d
            }
        },
        Err(_) => std::ptr::null_mut(),
    };

    let ozone = unsafe { jfn_platform_cef_ozone_platform() };
    if !unsafe { jfn_wl_dmabuf_probe(ozone, egl_dpy) } {
        log::warn!("Shared textures not supported; using software CEF rendering");
        unsafe { jfn_platform_set_shared_texture_unsupported() };
    }

    #[cfg(feature = "kde-palette")]
    unsafe { crate::kde_palette::jfn_wl_kde_palette_attach(display, parent) };

    crate::input_lifecycle::lifecycle_start();

    crate::clipboard::clipboard_init();
    if !crate::clipboard::clipboard_available() {
        unsafe { jfn_platform_clear_clipboard_handler() };
    }

    true
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_wl_lifecycle_cleanup() {
    crate::fade::jfn_wl_fade_stop_all();

    // Null the close trampoline before tearing down state it would read.
    if let Some(slot) = mpv_prop_intptr(c"wayland-close-cb-ptr") {
        unsafe { write_close_cb(slot, None) };
    }

    // KDE palette: KWin atomically drops the palette object with the
    // window. The scheme file is unlinked separately via
    // jfn_wl_kde_palette_post_window_cleanup after mpv tears down the
    // surface.
    jfn_idle_inhibit_linux::cleanup();
    crate::clipboard::clipboard_cleanup();
    crate::input_lifecycle::lifecycle_cleanup();
    // Rust-side WlState lives until process exit (mirrors C++ globals).
}

// Borrowed from jfn_playback — declared here to avoid a workspace dep.
unsafe extern "C" {
    fn jfn_playback_fullscreen() -> bool;
}
