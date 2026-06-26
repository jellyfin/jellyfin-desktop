//! Native [`WindowSource`]: the Wayland backend owns the toplevel, so live
//! geometry comes from compositor state, not mpv ingest.

use jfn_platform_abi::{PhysicalSize, WindowPos, WindowSource};

pub struct WaylandWindowSource;

impl WindowSource for WaylandWindowSource {
    fn size(&self) -> Option<PhysicalSize> {
        crate::proxy::jfn_wl_window_size_known().then(|| {
            let (w, h) = crate::proxy::jfn_wl_window_size();
            PhysicalSize::new(w, h)
        })
    }

    fn maximized(&self) -> bool {
        crate::proxy::jfn_wl_window_maximized()
    }

    fn fullscreen(&self) -> bool {
        crate::proxy::jfn_wl_window_fullscreen()
    }

    fn position(&self) -> Option<WindowPos> {
        None
    }

    fn scale(&self) -> f64 {
        jfn_platform_abi::scale_get().unwrap_or(0.0)
    }
}
