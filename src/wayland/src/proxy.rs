//! Cached preferred-scale value + proxy-callback wiring.
//!
//! Owns the `cached_scale` that used to live in WlState on the C++ side and
//! the scale-callback registered against jfn-wlproxy. The configure callback
//! stays in C++ (the runtime resize path it drives still lives in
//! wayland.cpp) — Rust just forwards its pointer through to wlproxy.
//!
//! Storage: `AtomicU32` holding the f32 bits, so reads from C++ getter
//! callbacks (any thread) don't need a mutex. Zero bits sentinel for
//! "scale unknown" — same semantics as the C++ `cached_scale = 0.0f` flag.

use std::ffi::c_int;
use std::sync::atomic::{AtomicU32, Ordering};

type JfnConfigureCb = extern "C" fn(c_int, c_int, c_int);
type JfnScaleCb = extern "C" fn(c_int);

// Declared in jfn-wlproxy (src/wlproxy/wlproxy.h); brought in here as
// extern decls to avoid a workspace cycle (the jfn-wlproxy crate doesn't
// expose its private callback type aliases).
unsafe extern "C" {
    fn jfn_wlproxy_set_configure_callback(cb: JfnConfigureCb);
    fn jfn_wlproxy_set_scale_callback(cb: JfnScaleCb);
}

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

#[unsafe(no_mangle)]
pub extern "C" fn jfn_wl_register_proxy_callbacks(configure_cb: JfnConfigureCb) {
    unsafe {
        jfn_wlproxy_set_configure_callback(configure_cb);
        jfn_wlproxy_set_scale_callback(on_scale);
    }
}
