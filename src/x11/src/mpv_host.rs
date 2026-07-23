use jfn_platform_abi::{MpvHost, WindowDecorations};

pub struct X11MpvHost;

impl MpvHost for X11MpvHost {
    fn prepare(&self, _configured: Option<WindowDecorations>) {
        if !crate::mpv_proxy::start() {
            tracing::error!(target: "Main", "x11 mpv proxy failed to start; mpv will connect directly");
        }
    }
}
