//! Wayland-backend `Platform::init` / `Platform::cleanup` body.
//!
//! Drives the per-process Wayland subsystems in order: read mpv's
//! wayland-display and -surface handles, prime the cached fullscreen,
//! wire input, bring up the core state, install mpv's close-cb
//! trampoline, init EGL, probe dmabuf support, attach the KDE palette
//! manager, start the input thread, and bring up the clipboard reader.

use std::ffi::{CStr, c_void};

use jfn_linux_util::egl_dyn as egl;

// =====================================================================
// FFI declarations consumed during init/cleanup.
// =====================================================================

use jfn_linux_util::dmabuf_probe::jfn_wl_dmabuf_probe;
use jfn_mpv::api::jfn_mpv_get_property_int;
use jfn_playback::shutdown::jfn_shutdown_initiate;

// =====================================================================
// Helpers
// =====================================================================

fn paint_name(mode: crate::paint_override::WlPaintOverride) -> &'static str {
    use crate::paint_override::WlPaintOverride as M;
    match mode {
        M::Dmabuf => "dmabuf",
        M::Gpu => "gpu",
        M::Shm => "shm",
    }
}

fn mpv_prop_intptr(name: &CStr) -> Option<usize> {
    let mut v: i64 = 0;
    let rc = unsafe { jfn_mpv_get_property_int(name.as_ptr(), &mut v) };
    if rc == 0 && v != 0 {
        Some(v as usize)
    } else {
        None
    }
}

// These requried properties are non-upstream
// Use https://github.com/andrewrabert/mpv/tree/cef-mpv
fn nonupstream_wayland_hooks_present() -> bool {
    const HOOKS: [&CStr; 3] = [c"wayland-display", c"wayland-surface", c"wayland-close-cb-ptr"];
    let mut missing: Vec<&str> = Vec::new();
    for name in HOOKS {
        let mut v: i64 = 0;
        let rc = unsafe { jfn_mpv_get_property_int(name.as_ptr(), &mut v) };
        if rc == jfn_mpv::sys::mpv_error::MPV_ERROR_PROPERTY_NOT_FOUND.0 {
            missing.push(name.to_str().unwrap_or("?"));
        }
    }
    if missing.is_empty() {
        return true;
    }
    tracing::error!(?missing, "non-upstream mpv Wayland embedding hooks absent");
    false
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
    jfn_shutdown_initiate();
}

// =====================================================================
// init / cleanup
// =====================================================================

pub fn jfn_wl_lifecycle_init() -> bool {
    if !nonupstream_wayland_hooks_present() {
        tracing::error!("Wayland embedding hooks missing");
        return false;
    }

    let Some(display) = mpv_prop_intptr(c"wayland-display").map(|p| p as *mut c_void) else {
        tracing::error!("Failed to get Wayland display from mpv");
        return false;
    };
    let Some(parent) = mpv_prop_intptr(c"wayland-surface").map(|p| p as *mut c_void) else {
        tracing::error!("Failed to get Wayland surface from mpv");
        return false;
    };

    // Seed Rust state with mpv's current fullscreen — first configure
    // after this point won't start a spurious transition.
    crate::wl_ffi::jfn_wl_core_set_was_fullscreen(
        jfn_playback::ingest_driver::jfn_playback_fullscreen(),
    );

    // Prepare the input layer first so its xkb context is ready before
    // any seat_caps wires up keyboard listeners that need xkb.
    crate::input_lifecycle::lifecycle_init(display);

    if !unsafe { crate::wl_ffi::jfn_wl_core_init(display, parent) } {
        tracing::error!("jfn_wl_core_init failed");
        return false;
    }

    // Register close callback — intercepts xdg_toplevel close before mpv
    // sees it. mpv exposes the slot as a `(fn-ptr, data-ptr)` pair packed
    // at the address it returns through `wayland-close-cb-ptr`.
    if let Some(slot) = mpv_prop_intptr(c"wayland-close-cb-ptr") {
        unsafe { write_close_cb(slot, Some(close_cb_trampoline)) };
    }

    use crate::paint_override::WlPaintOverride as Req;
    let requested = crate::paint_override::paint_override();
    let explicit = requested.is_some();
    let entry = requested.unwrap_or(Req::Dmabuf);

    let mut want_gpu_paint = false;
    let mut resolved = Req::Shm;
    match entry {
        Req::Shm => {
            tracing::info!("paint: using wl_shm");
            jfn_platform_abi::get().set_shared_texture_unsupported();
        }
        Req::Gpu => {
            tracing::info!("paint: Vulkan WSI pixel-upload");
            jfn_platform_abi::get().set_shared_texture_unsupported();
            want_gpu_paint = true;
            resolved = Req::Gpu;
        }
        Req::Dmabuf => {
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

            let ozone = jfn_platform_abi::get().cef_ozone_platform();
            if unsafe { jfn_wl_dmabuf_probe(ozone, egl_dpy) } {
                tracing::info!("paint: EGL/GBM dmabuf shared texture");
                resolved = Req::Dmabuf;
            } else {
                tracing::info!("paint: EGL dmabuf unavailable; trying gpu");
                jfn_platform_abi::get().set_shared_texture_unsupported();
                want_gpu_paint = true;
                resolved = Req::Gpu;
            }
        }
    }

    if want_gpu_paint {
        match jfn_gpu_paint::GpuContext::new() {
            Ok(ctx) => {
                crate::wl_state::install_gpu_paint(ctx);
            }
            Err(e) => {
                tracing::info!("paint: Vulkan init failed: {e}; using wl_shm");
                resolved = Req::Shm;
            }
        }
    }

    if explicit && requested != Some(resolved) {
        tracing::warn!(
            "--platform-paint={} unavailable; using {}",
            paint_name(requested.unwrap()),
            paint_name(resolved)
        );
    }

    #[cfg(feature = "kde-palette")]
    unsafe {
        crate::kde_palette::jfn_wl_kde_palette_attach(display, parent)
    };

    crate::input_lifecycle::lifecycle_start();

    crate::clipboard::clipboard_init();
    if !crate::clipboard::clipboard_available() {
        jfn_platform_abi::get().clear_clipboard_handler();
    }

    true
}

pub fn jfn_wl_lifecycle_cleanup() {
    // Null the close trampoline before tearing down state it would read.
    if let Some(slot) = mpv_prop_intptr(c"wayland-close-cb-ptr") {
        unsafe { write_close_cb(slot, None) };
    }

    // KDE palette: KWin atomically drops the palette object with the
    // window. The scheme file is unlinked separately via
    // jfn_wl_kde_palette_post_window_cleanup after mpv tears down the
    // surface.
    jfn_linux_util::idle_inhibit::cleanup();
    crate::clipboard::clipboard_cleanup();
    crate::input_lifecycle::lifecycle_cleanup();
    // Rust-side WlState lives until process exit (mirrors C++ globals).
}
