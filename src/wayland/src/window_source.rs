//! Native [`WindowSource`]: the Wayland backend owns the toplevel, so live
//! geometry comes from compositor state, not mpv ingest.

use jfn_platform_abi::{
    LogicalSize, PhysicalSize, Scale, WindowExtent, WindowSnapshot, WindowSource,
};

pub struct WaylandWindowSource;

impl WindowSource for WaylandWindowSource {
    fn snapshot(&self) -> WindowSnapshot {
        // One snapshot so extent and mode can't span two generations.
        let snap = crate::window_state::window_extent();
        WindowSnapshot {
            extent: snap.map(|e| {
                WindowExtent::with_logical(
                    PhysicalSize {
                        w: e.physical().w(),
                        h: e.physical().h(),
                    },
                    Scale(e.scale()),
                    LogicalSize {
                        w: e.logical().w(),
                        h: e.logical().h(),
                    },
                )
            }),
            position: None,
            maximized: snap.is_some_and(|e| e.mode() == crate::window_state::WindowMode::Maximized),
            fullscreen: snap
                .is_some_and(|e| e.mode() == crate::window_state::WindowMode::Fullscreen),
        }
    }
}
