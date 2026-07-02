//! Native [`WindowSource`]: the Wayland backend owns the toplevel, so live
//! geometry comes from compositor state, not mpv ingest.

use jfn_platform_abi::{PhysicalSize, Scale, WindowPos, WindowSource};

pub struct WaylandWindowSource;

impl WindowSource for WaylandWindowSource {
    fn size(&self) -> Option<PhysicalSize> {
        crate::window_state::jfn_wl_window_size_known().then(|| {
            let (w, h) = crate::window_state::jfn_wl_window_size();
            PhysicalSize { w, h }
        })
    }

    fn maximized(&self) -> bool {
        crate::window_state::jfn_wl_window_maximized()
    }

    fn fullscreen(&self) -> bool {
        crate::window_state::jfn_wl_window_fullscreen()
    }

    fn position(&self) -> Option<WindowPos> {
        None
    }

    fn scale(&self) -> Scale {
        Scale(crate::window_state::jfn_wl_get_cached_scale())
    }
}
