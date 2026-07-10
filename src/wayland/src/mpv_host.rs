//! Wayland [`MpvHost`]: starts the proxy mpv connects to in place of the
//! compositor, and drives the app-owned root window.

use std::ffi::CStr;
use std::sync::OnceLock;

use crate::mpv_proxy::{display_name, start, stop};
use jfn_platform_abi::{MpvHost, WindowDecorations};

static PROXY: OnceLock<ProxySlot> = OnceLock::new();

struct ProxySlot(*mut crate::mpv_proxy::Proxy);
unsafe impl Send for ProxySlot {}
unsafe impl Sync for ProxySlot {}

pub struct WaylandMpvHost;

impl MpvHost for WaylandMpvHost {
    fn prepare(&self, decorations: WindowDecorations) {
        unsafe { start_proxy(decorations) };
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

unsafe fn start_proxy(decorations: WindowDecorations) {
    let p = start();
    if p.is_null() {
        tracing::error!(target: "Main", "proxy start failed; continuing without proxy");
        return;
    }
    let disp_p = unsafe { display_name(p) };
    if disp_p.is_null() {
        tracing::error!(target: "Main", "proxy display name empty; aborting proxy");
        unsafe { stop(p) };
        return;
    }
    let disp = unsafe { CStr::from_ptr(disp_p) }
        .to_string_lossy()
        .into_owned();
    if disp.is_empty() {
        tracing::error!(target: "Main", "proxy display name empty; aborting proxy");
        unsafe { stop(p) };
        return;
    }
    tracing::info!(target: "Main", "proxy listening on {disp}");
    crate::root_window::set_decorations(decorations);
    unsafe { std::env::set_var("WAYLAND_DISPLAY", &disp) };
    let _ = PROXY.set(ProxySlot(p));
}

pub(crate) fn stop_proxy() {
    if let Some(slot) = PROXY.get() {
        unsafe { stop(slot.0) };
    }
}
