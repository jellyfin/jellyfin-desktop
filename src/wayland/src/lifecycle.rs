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

fn vo_captured_display() -> Option<*mut c_void> {
    let idx = jfn_wlproxy::jfn_wlproxy_vo_connection_index();
    let count = jfn_linux_util::wl_display_registry::connect_count();
    tracing::info!(target: "WlInterpose", "vo_captured_display: vo_index={idx} captured_count={count}");
    if idx < 0 {
        return None;
    }
    let d = jfn_linux_util::wl_display_registry::captured_display(idx as usize);
    if !d.is_null() {
        return Some(d);
    }
    // The wlproxy's SAME_PROC_SEQ index can be 1 ahead of the registry on
    // some GPU contexts (e.g. EGL Wayland) where mpv's internal connection
    // sequence differs from the Vulkan path. Fall back to the last captured
    // display, which is mpv's VO connection in this case.
    if count > 0 {
        let last = jfn_linux_util::wl_display_registry::captured_display(count - 1);
        if !last.is_null() {
            tracing::warn!(target: "WlInterpose", "vo_index out of range; falling back to last captured display (idx {})", count - 1);
            return Some(last);
        }
    }
    None
}

// =====================================================================
// init / cleanup
// =====================================================================

pub fn jfn_wl_lifecycle_init() -> bool {
    let captured = vo_captured_display();
    let prop = mpv_prop_intptr(c"wayland-display").map(|p| p as *mut c_void);
    tracing::info!(
        target: "WlInterpose",
        "vo display: captured={:?} wayland-display={:?} match={}",
        captured,
        prop,
        captured.is_some() && captured == prop
    );

    let display = match (captured, prop) {
        (Some(d), _) => d,
        (None, Some(d)) => {
            tracing::warn!(
                target: "WlInterpose",
                "interposer capture unavailable; falling back to wayland-display property"
            );
            d
        }
        (None, None) => {
            tracing::error!("Failed to get Wayland display (no capture, no property)");
            return false;
        }
    };

    // Seed Rust state with mpv's current fullscreen — first configure
    // after this point won't start a spurious transition.
    crate::wl_ffi::jfn_wl_core_set_was_fullscreen(
        jfn_playback::ingest_driver::jfn_playback_fullscreen(),
    );

    // Prepare the input layer first so its xkb context is ready before
    // any seat_caps wires up keyboard listeners that need xkb.
    crate::input_lifecycle::lifecycle_init(display);

    if !unsafe { crate::wl_ffi::jfn_wl_core_init(display) } {
        tracing::error!("jfn_wl_core_init failed");
        return false;
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

            if unsafe { jfn_wl_dmabuf_probe(c"wayland".as_ptr(), egl_dpy) } {
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
        match jfn_gpu_paint::GpuContext::new(jfn_gpu_paint::GpuTarget::default()) {
            Ok(ctx) => {
                crate::wl_state::install_gpu_paint(ctx);
            }
            Err(e) => {
                tracing::info!("paint: Vulkan init failed: {e}; using wl_shm");
                resolved = Req::Shm;
            }
        }
    }

    if explicit
        && let Some(req) = requested
        && req != resolved
    {
        tracing::warn!(
            "--platform-paint={} unavailable; using {}",
            paint_name(req),
            paint_name(resolved)
        );
    }

    #[cfg(feature = "kde-palette")]
    crate::kde_palette::jfn_wl_kde_palette_init();

    crate::input_lifecycle::lifecycle_start();

    crate::clipboard::clipboard_init();
    if !crate::clipboard::clipboard_available() {
        jfn_platform_abi::get().clear_clipboard_handler();
    }

    true
}

pub fn jfn_wl_lifecycle_cleanup() {
    // KDE palette: KWin atomically drops the palette object with the
    // window. The scheme file is unlinked separately via
    // jfn_wl_kde_palette_post_window_cleanup after mpv tears down the
    // surface.
    jfn_linux_util::idle_inhibit::cleanup();
    crate::clipboard::clipboard_cleanup();
    crate::input_lifecycle::lifecycle_cleanup();
    // Rust-side WlState lives until process exit (mirrors C++ globals).
}
