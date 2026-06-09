//! Cached preferred-scale value + proxy-callback wiring.
//!
//! Owns the `cached_scale` and the scale-callback registered against
//! jfn-wlproxy. Also owns the xdg_toplevel.configure intercept that
//! forwards into the runtime resize path (`wl_ops::on_configure`) and
//! pushes synthetic OSD-dim pixels into the playback coordinator.
//!
//! Storage: `AtomicU32` holding the f32 bits, so reads from any thread
//! don't need a mutex. Zero bits sentinel for
//! "scale unknown" — same semantics as the C++ `cached_scale = 0.0f` flag.

use std::ffi::c_int;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use crate::wl_ops;

use jfn_playback::ingest_driver::jfn_playback_post_osd_pixels;
use jfn_wlproxy::{
    jfn_wlproxy_set_close_callback, jfn_wlproxy_set_configure_callback,
    jfn_wlproxy_set_popup_done_callback, jfn_wlproxy_set_popup_ready_callback,
    jfn_wlproxy_set_scale_callback, jfn_wlproxy_set_suspended_callback,
};

/// Compositor-reported window state, fed by the configure + scale intercepts
/// and read by the host's `WindowSource`. `scale_bits`/`size` use 0 as the
/// "unknown" sentinel — safe because the intercepts reject non-positive values.
struct WlWindowState {
    scale_bits: AtomicU32,
    size: AtomicU64,
    maximized: AtomicBool,
    fullscreen: AtomicBool,
}

static WINDOW_STATE: WlWindowState = WlWindowState {
    scale_bits: AtomicU32::new(0),
    size: AtomicU64::new(0),
    maximized: AtomicBool::new(false),
    fullscreen: AtomicBool::new(false),
};

impl WlWindowState {
    fn set_scale(&self, s: f32) {
        self.scale_bits.store(s.to_bits(), Ordering::Release);
    }
    fn scale(&self) -> f32 {
        f32::from_bits(self.scale_bits.load(Ordering::Acquire))
    }
    fn set_size(&self, w: c_int, h: c_int) {
        self.size.store(
            ((w as u32 as u64) << 32) | (h as u32 as u64),
            Ordering::Release,
        );
    }
    fn size(&self) -> (c_int, c_int) {
        let packed = self.size.load(Ordering::Acquire);
        (((packed >> 32) as u32) as c_int, (packed as u32) as c_int)
    }
}

extern "C" fn on_scale(scale_120: c_int) {
    if scale_120 > 0 {
        WINDOW_STATE.set_scale(scale_120 as f32 / 120.0);
        // Wake any thread parked in `mpv_wait_event` (the boot-time VO-wait
        // loop in `jfn_rust::app`) so it re-checks the scale-known gate
        // event-driven rather than via a polling timeout.
        jfn_mpv::api::jfn_mpv_wakeup();
    }
}

pub fn jfn_wl_scale_known() -> bool {
    WINDOW_STATE.scale() > 0.0
}

pub fn jfn_wl_get_cached_scale() -> f32 {
    let s = WINDOW_STATE.scale();
    if s > 0.0 { s } else { 1.0 }
}

pub fn jfn_wl_window_size_known() -> bool {
    WINDOW_STATE.size.load(Ordering::Acquire) != 0
}

pub fn jfn_wl_window_size() -> (c_int, c_int) {
    WINDOW_STATE.size()
}

pub fn jfn_wl_window_maximized() -> bool {
    WINDOW_STATE.maximized.load(Ordering::Acquire)
}

pub fn jfn_wl_window_fullscreen() -> bool {
    WINDOW_STATE.fullscreen.load(Ordering::Acquire)
}

// xdg_toplevel.configure intercept — fires on the wl-proxy per-client thread
// for every configure from the compositor. Authoritative size source on
// Wayland. Forwards into `wl_ops::on_configure` (which is a no-op until
// `jfn_wl_core_init` has run; see jfn_wl_on_configure) and posts synthetic
// OSD-dim pixels through the playback coordinator.
extern "C" fn on_configure(
    physical_w: c_int,
    physical_h: c_int,
    fullscreen: c_int,
    maximized: c_int,
) {
    if physical_w <= 0 || physical_h <= 0 {
        return;
    }
    WINDOW_STATE.set_size(physical_w, physical_h);
    WINDOW_STATE
        .maximized
        .store(maximized != 0, Ordering::Release);
    WINDOW_STATE
        .fullscreen
        .store(fullscreen != 0, Ordering::Release);
    crate::wl_ffi::sync_maximized_command_state(maximized != 0);
    let scale = if crate::wl_state::try_state().is_some() {
        let s = WINDOW_STATE.scale();
        if s > 0.0 { s } else { 1.0 }
    } else {
        1.0
    };
    if crate::wl_state::try_state().is_some() {
        wl_ops::on_configure(physical_w, physical_h, fullscreen != 0, scale);
    }
    jfn_playback_post_osd_pixels(physical_w, physical_h, scale, false, 0, 0);
    // Wake any thread parked in `mpv_wait_event` (the boot-time VO-wait
    // loop reads OSD pixels from the ingest layer rather than via an mpv
    // event, so a synthetic configure that lands while main is blocked
    // would otherwise go unobserved).
    jfn_mpv::api::jfn_mpv_wakeup();
}

extern "C" fn on_suspended(suspended: c_int) {
    jfn_playback::lifecycle::jfn_lifecycle_set_visible(suspended == 0);
}

extern "C" fn on_close() {
    jfn_playback::shutdown::jfn_shutdown_initiate();
}

extern "C" fn on_popup_ready() {
    crate::popup::on_ready();
}

extern "C" fn on_popup_done() {
    crate::popup::on_done();
}

pub fn jfn_wl_register_proxy_callbacks() {
    jfn_wlproxy_set_configure_callback(on_configure);
    jfn_wlproxy_set_scale_callback(on_scale);
    jfn_wlproxy_set_suspended_callback(on_suspended);
    jfn_wlproxy_set_popup_ready_callback(on_popup_ready);
    jfn_wlproxy_set_popup_done_callback(on_popup_done);
    jfn_wlproxy_set_close_callback(on_close);
}
