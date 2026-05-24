//! Cached preferred-scale value + proxy-callback wiring.
//!
//! Owns the `cached_scale` that used to live in WlState on the C++ side and
//! the scale-callback registered against jfn-wlproxy. Also owns the
//! xdg_toplevel.configure intercept that forwards into the runtime resize
//! path (`wl_ops::on_configure`) and pushes synthetic OSD-dim pixels into
//! the playback coordinator.
//!
//! Storage: `AtomicU32` holding the f32 bits, so reads from C++ getter
//! callbacks (any thread) don't need a mutex. Zero bits sentinel for
//! "scale unknown" — same semantics as the C++ `cached_scale = 0.0f` flag.

use std::ffi::c_int;
use std::sync::atomic::{AtomicU32, Ordering};

use crate::wl_ops;

use jfn_playback::ingest_driver::jfn_playback_post_osd_pixels;
use jfn_wlproxy::{jfn_wlproxy_set_configure_callback, jfn_wlproxy_set_scale_callback};

static CACHED_SCALE_BITS: AtomicU32 = AtomicU32::new(0);

fn store_scale(s: f32) {
    CACHED_SCALE_BITS.store(s.to_bits(), Ordering::Release);
}

fn load_scale() -> f32 {
    f32::from_bits(CACHED_SCALE_BITS.load(Ordering::Acquire))
}

extern "C" fn on_scale(scale_120: c_int) {
    if scale_120 > 0 {
        store_scale(scale_120 as f32 / 120.0);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_wl_scale_known() -> bool {
    load_scale() > 0.0
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_wl_get_cached_scale() -> f32 {
    let s = load_scale();
    if s > 0.0 { s } else { 1.0 }
}

// xdg_toplevel.configure intercept — fires on the wl-proxy per-client thread
// for every configure from the compositor. Authoritative size source on
// Wayland. Forwards into `wl_ops::on_configure` (which is a no-op until
// `jfn_wl_core_init` has run; see jfn_wl_on_configure) and posts synthetic
// OSD-dim pixels through the playback coordinator.
extern "C" fn on_configure(physical_w: c_int, physical_h: c_int, fullscreen: c_int) {
    if physical_w <= 0 || physical_h <= 0 {
        return;
    }
    let scale = if crate::wl_state::try_state().is_some() {
        let s = load_scale();
        if s > 0.0 { s } else { 1.0 }
    } else {
        1.0
    };
    if crate::wl_state::try_state().is_some() {
        wl_ops::on_configure(physical_w, physical_h, fullscreen != 0, scale);
    }
    jfn_playback_post_osd_pixels(physical_w, physical_h, scale, false, 0, 0);
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_wl_register_proxy_callbacks() {
    unsafe {
        jfn_wlproxy_set_configure_callback(on_configure);
        jfn_wlproxy_set_scale_callback(on_scale);
    }
}
