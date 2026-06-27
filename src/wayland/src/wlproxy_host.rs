//! Wayland [`MpvHost`]: wlproxy owns the toplevel mpv connects to.
//!
//! `prepare` starts the proxy and points mpv's `WAYLAND_DISPLAY` at it
//! before `mpv_create`, so the first compositor configure (which arrives
//! shortly after `mpv_initialize`) is intercepted. The proxy is stopped
//! from `Platform::post_window_cleanup` via [`stop_wlproxy`].

use std::ffi::CStr;
use std::sync::OnceLock;

use jfn_platform_abi::{MpvHost, WindowDecorations};
use jfn_wlproxy::{jfn_wlproxy_display_name, jfn_wlproxy_start, jfn_wlproxy_stop};

static WLPROXY: OnceLock<WlproxySlot> = OnceLock::new();

struct WlproxySlot(*mut jfn_wlproxy::Proxy);
unsafe impl Send for WlproxySlot {}
unsafe impl Sync for WlproxySlot {}

pub struct WlproxyMpvHost;

impl MpvHost for WlproxyMpvHost {
    fn prepare(&self, decorations: WindowDecorations) {
        unsafe { start_wlproxy(decorations) };
    }

    fn host_ready(&self) -> bool {
        crate::window_state::jfn_wl_scale_known()
    }

    fn window_maximized(&self) -> Option<bool> {
        Some(crate::window_state::jfn_wl_window_maximized())
    }

    fn ensure_host_window(&self) {
        crate::root_window::ensure_started();
    }

    fn detach(&self) {}
}

unsafe fn start_wlproxy(decorations: WindowDecorations) {
    let p = jfn_wlproxy_start();
    if p.is_null() {
        tracing::error!(target: "Main", "wlproxy start failed; continuing without proxy");
        return;
    }
    let disp_p = unsafe { jfn_wlproxy_display_name(p) };
    if disp_p.is_null() {
        tracing::error!(target: "Main", "wlproxy display name empty; aborting proxy");
        unsafe { jfn_wlproxy_stop(p) };
        return;
    }
    let disp = unsafe { CStr::from_ptr(disp_p) }
        .to_string_lossy()
        .into_owned();
    if disp.is_empty() {
        tracing::error!(target: "Main", "wlproxy display name empty; aborting proxy");
        unsafe { jfn_wlproxy_stop(p) };
        return;
    }
    tracing::info!(target: "Main", "wlproxy listening on {disp}");
    let deco_mode = match decorations {
        WindowDecorations::Csd => 1,
        WindowDecorations::Server => 2,
        WindowDecorations::ServerThemed => 3,
    };
    crate::root_window::set_decorations(deco_mode as u32);
    unsafe { std::env::set_var("WAYLAND_DISPLAY", &disp) };
    let _ = WLPROXY.set(WlproxySlot(p));
}

/// Stop the proxy started by `prepare`. Idempotent against a proxy that
/// never started.
pub(crate) fn stop_wlproxy() {
    if let Some(slot) = WLPROXY.get() {
        unsafe { jfn_wlproxy_stop(slot.0) };
    }
}
