//! Native [`WindowSource`]: the Wayland backend owns the toplevel, so live
//! geometry comes from compositor state, not mpv ingest.

use jfn_platform_abi::{PhysicalSize, Scale, WindowExtent, WindowSnapshot, WindowSource};

pub struct WaylandWindowSource;

impl WindowSource for WaylandWindowSource {
    fn snapshot(&self) -> WindowSnapshot {
        let extent = crate::window_state::window_extent().map(|e| {
            WindowExtent::new(
                PhysicalSize {
                    w: e.physical().w(),
                    h: e.physical().h(),
                },
                Scale(e.scale()),
            )
        });
        WindowSnapshot {
            extent,
            position: None,
            maximized: crate::window_state::jfn_wl_window_maximized(),
            fullscreen: crate::window_state::jfn_wl_window_fullscreen(),
        }
    }
}
