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

use crate::proxy::jfn_wl_register_proxy_callbacks;

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
        jfn_platform_abi::scale_runtime_known()
    }

    fn detach(&self) {
        // Sever the wlproxy→host callbacks before CEF teardown. A CEF
        // paint thread can be terminated by `jfn_cef_shutdown` while
        // holding the WlState lock, orphaning it; if the proxy's
        // mpv-connection thread then runs `on_configure` it parks on that
        // lock forever and can no longer forward mpv's VO-teardown
        // roundtrip, deadlocking the whole shutdown when video was
        // playing.
        jfn_wlproxy::jfn_wlproxy_clear_callbacks();
    }
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
    jfn_wlproxy::jfn_wlproxy_set_decoration_mode(deco_mode);
    unsafe { std::env::set_var("WAYLAND_DISPLAY", &disp) };
    // Register the configure intercept BEFORE mpv_create so the first
    // compositor configure (which arrives shortly after mpv_initialize) is
    // captured.
    jfn_wl_register_proxy_callbacks();
    let _ = WLPROXY.set(WlproxySlot(p));
}

/// Stop the proxy started by `prepare`. Idempotent against a proxy that
/// never started.
pub(crate) fn stop_wlproxy() {
    if let Some(slot) = WLPROXY.get() {
        unsafe { jfn_wlproxy_stop(slot.0) };
    }
}
