//! Window-geometry lifecycle owner: boot restore, live state, exit persist.
//!
//! Live state comes from one [`WindowSource`] — Wayland reads native compositor
//! state, mpv-backed backends (macOS / Windows / X11) read mpv ingest.

use std::sync::OnceLock;

use jfn_platform_abi::{
    BootGeometry, LogicalSize, PhysicalSize, Platform, Scale, WindowGeometry, WindowPos,
    WindowSource,
};

use jfn_config::JfnWindowGeometry;

const DEFAULT_LOGICAL: LogicalSize = LogicalSize { w: 1600, h: 900 };
const MIN_LOGICAL: LogicalSize = LogicalSize { w: 320, h: 180 };

fn plat() -> &'static dyn Platform {
    jfn_platform_abi::get()
}

struct MpvWindowSource;

impl WindowSource for MpvWindowSource {
    fn size(&self) -> Option<PhysicalSize> {
        let mut w = jfn_playback::ingest_driver::jfn_playback_window_pw();
        let mut h = jfn_playback::ingest_driver::jfn_playback_window_ph();
        if w <= 0 || h <= 0 {
            w = jfn_playback::ingest_driver::jfn_playback_osd_pw();
            h = jfn_playback::ingest_driver::jfn_playback_osd_ph();
        }
        (w > 0 && h > 0).then_some(PhysicalSize { w, h })
    }

    fn maximized(&self) -> bool {
        jfn_playback::ingest_driver::jfn_playback_window_maximized()
    }

    fn fullscreen(&self) -> bool {
        jfn_playback::ingest_driver::jfn_playback_fullscreen()
    }

    fn position(&self) -> Option<WindowPos> {
        plat().query_window_position()
    }

    fn scale(&self) -> Scale {
        Scale(plat().get_scale())
    }
}

static MPV_SOURCE: MpvWindowSource = MpvWindowSource;

/// Owns the boot→live→persist lifecycle for window geometry.
pub struct WindowGeometryController {
    source: &'static dyn WindowSource,
}

impl WindowGeometryController {
    fn new() -> Self {
        Self {
            source: plat().window_source().unwrap_or(&MPV_SOURCE),
        }
    }

    pub fn source(&self) -> &dyn WindowSource {
        self.source
    }

    /// Resolve saved config into typed boot geometry, sourcing the display
    /// scale + clamp from the platform.
    pub fn boot(&self) -> BootGeometry {
        let mut g = jfn_config::window_geometry();

        match jfn_config::startup_window_mode() {
            jfn_config::StartupWindowMode::Windowed => {
                g.maximized = false;
            }
            jfn_config::StartupWindowMode::Maximized => {
                g.maximized = true;
            }
            jfn_config::StartupWindowMode::Fullscreen => {
                g.maximized = false;
            }
        }

        let scale = Scale(plat().get_display_scale(g.x, g.y));
        resolve_boot(g, scale, |w| plat().clamp_window_geometry(w))
    }

    /// Read live state and write it back to config. Called at teardown before
    /// any thread-join that could hang.
    pub fn persist(&self) {
        let was_max_before_fs =
            jfn_playback::browser_sink::jfn_playback_was_maximized_before_fullscreen();
        if let Some(g) = geometry_to_persist(
            self.source(),
            jfn_config::window_geometry(),
            was_max_before_fs,
        ) {
            jfn_config::set_window_geometry(g);
        }
    }
}

/// Pure core of [`WindowGeometryController::boot`]: saved config + display scale
/// + a clamp fn → typed boot geometry. No globals, so it's unit-testable.
fn resolve_boot(
    g: JfnWindowGeometry,
    scale: Scale,
    clamp: impl Fn(WindowGeometry) -> WindowGeometry,
) -> BootGeometry {
    let logical = if g.logical_width >= MIN_LOGICAL.w && g.logical_height >= MIN_LOGICAL.h {
        LogicalSize {
            w: g.logical_width,
            h: g.logical_height,
        }
    } else if g.width >= MIN_LOGICAL.w && g.height >= MIN_LOGICAL.h {
        LogicalSize {
            w: g.width,
            h: g.height,
        }
    } else {
        DEFAULT_LOGICAL
    };
    let scale = scale.or_one();
    let physical = logical.to_physical(scale);
    // clamp operates on physical backing pixels; on Wayland it's the identity,
    // so the logical size we seed the toplevel with is unaffected.
    let clamped = clamp(WindowGeometry {
        w: physical.w,
        h: physical.h,
        x: g.x,
        y: g.y,
    });
    let position = (clamped.x >= 0 && clamped.y >= 0).then_some(WindowPos {
        x: clamped.x,
        y: clamped.y,
    });
    BootGeometry {
        logical,
        physical: PhysicalSize {
            w: clamped.w,
            h: clamped.h,
        },
        scale,
        position,
        maximized: g.maximized,
    }
}

pub fn controller() -> &'static WindowGeometryController {
    static CONTROLLER: OnceLock<WindowGeometryController> = OnceLock::new();
    CONTROLLER.get_or_init(WindowGeometryController::new)
}

/// Returns `None` when size is unknown, so the caller doesn't overwrite saved
/// geometry with zeros.
fn geometry_to_persist(
    ws: &dyn WindowSource,
    saved: JfnWindowGeometry,
    was_maximized_before_fullscreen: bool,
) -> Option<JfnWindowGeometry> {
    if ws.fullscreen() {
        let mut g = saved;
        g.maximized = was_maximized_before_fullscreen;
        return Some(g);
    }
    if ws.maximized() {
        let mut g = saved;
        g.maximized = true;
        return Some(g);
    }
    let physical = ws.size()?;
    if physical.w <= 0 || physical.h <= 0 {
        return None;
    }
    let scale = ws.scale().or_one();
    let logical = physical.to_logical(scale);
    let pos = ws.position();
    Some(JfnWindowGeometry {
        width: physical.w,
        height: physical.h,
        scale: scale.0,
        logical_width: logical.w,
        logical_height: logical.h,
        maximized: false,
        x: pos.map_or(-1, |p| p.x),
        y: pos.map_or(-1, |p| p.y),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    struct FakeWindowSource {
        size: Option<PhysicalSize>,
        maximized: bool,
        fullscreen: bool,
        position: Option<WindowPos>,
        scale: Scale,
    }

    impl WindowSource for FakeWindowSource {
        fn size(&self) -> Option<PhysicalSize> {
            self.size
        }
        fn maximized(&self) -> bool {
            self.maximized
        }
        fn fullscreen(&self) -> bool {
            self.fullscreen
        }
        fn position(&self) -> Option<WindowPos> {
            self.position
        }
        fn scale(&self) -> Scale {
            self.scale
        }
    }

    fn fake(size: Option<PhysicalSize>, scale: f32) -> FakeWindowSource {
        FakeWindowSource {
            size,
            maximized: false,
            fullscreen: false,
            position: None,
            scale: Scale(scale),
        }
    }

    #[test]
    fn wayland_shaped_no_position_scaled() {
        let ws = fake(Some(PhysicalSize { w: 2400, h: 1350 }), 1.5);
        let g = geometry_to_persist(&ws, JfnWindowGeometry::default(), false).unwrap();
        assert_eq!((g.x, g.y), (-1, -1));
        assert_eq!((g.width, g.height), (2400, 1350));
        assert_eq!((g.logical_width, g.logical_height), (1600, 900));
        assert_eq!(g.scale, 1.5);
        assert!(!g.maximized);
    }

    #[test]
    fn mpv_shaped_with_position() {
        let ws = FakeWindowSource {
            position: Some(WindowPos { x: 100, y: 50 }),
            ..fake(Some(PhysicalSize { w: 1280, h: 720 }), 1.0)
        };
        let g = geometry_to_persist(&ws, JfnWindowGeometry::default(), false).unwrap();
        assert_eq!((g.x, g.y), (100, 50));
        assert_eq!((g.logical_width, g.logical_height), (1280, 720));
    }

    #[test]
    fn maximized_keeps_prior_size() {
        let saved = JfnWindowGeometry {
            width: 1280,
            height: 720,
            logical_width: 1280,
            logical_height: 720,
            scale: 1.0,
            ..Default::default()
        };
        let ws = FakeWindowSource {
            maximized: true,
            ..fake(Some(PhysicalSize { w: 300, h: 200 }), 1.0)
        };
        let g = geometry_to_persist(&ws, saved, false).unwrap();
        assert!(g.maximized);
        assert_eq!((g.width, g.height), (1280, 720));
    }

    #[test]
    fn fullscreen_preserves_pre_fullscreen_state() {
        let saved = JfnWindowGeometry {
            width: 1600,
            height: 900,
            ..Default::default()
        };
        let ws = FakeWindowSource {
            maximized: true,
            fullscreen: true,
            ..fake(Some(PhysicalSize { w: 3840, h: 2160 }), 1.0)
        };
        let g = geometry_to_persist(&ws, saved, true).unwrap();
        assert!(g.maximized);
        assert_eq!((g.width, g.height), (1600, 900));
    }

    #[test]
    fn unknown_size_returns_none() {
        let ws = fake(None, 1.0);
        assert!(geometry_to_persist(&ws, JfnWindowGeometry::default(), false).is_none());
    }

    #[test]
    fn logical_rounding() {
        for (scale, phys, logical) in [(1.25_f32, 2000, 1600), (2.0, 3000, 1500)] {
            let ws = fake(Some(PhysicalSize { w: phys, h: phys }), scale);
            let g = geometry_to_persist(&ws, JfnWindowGeometry::default(), false).unwrap();
            assert_eq!(g.logical_width, logical);
        }
    }

    fn identity_clamp(w: WindowGeometry) -> WindowGeometry {
        w
    }

    #[test]
    fn tiny_saved_size_falls_back_to_default() {
        let saved = JfnWindowGeometry {
            width: 232,
            height: 5,
            logical_width: 232,
            logical_height: 5,
            scale: 1.0,
            ..Default::default()
        };
        let boot = resolve_boot(saved, Scale(1.0), identity_clamp);
        assert_eq!(boot.logical, DEFAULT_LOGICAL);
        assert_eq!(boot.physical, DEFAULT_LOGICAL.to_physical(Scale(1.0)));
    }

    #[test]
    fn boot_restores_cross_scale() {
        let saved = JfnWindowGeometry {
            logical_width: 1280,
            logical_height: 720,
            scale: 1.0,
            ..Default::default()
        };
        let boot = resolve_boot(saved, Scale(1.25), identity_clamp);
        assert_eq!(boot.logical, LogicalSize { w: 1280, h: 720 });
        assert_eq!(boot.physical, PhysicalSize { w: 1600, h: 900 });
        assert!(boot.position.is_none());
    }

    #[test]
    fn maximize_round_trip_preserves_size() {
        // Live state: maximized; persist keeps the prior (pre-maximize) size.
        let prior = JfnWindowGeometry {
            logical_width: 1280,
            logical_height: 720,
            width: 1280,
            height: 720,
            scale: 1.0,
            ..Default::default()
        };
        let ws = FakeWindowSource {
            maximized: true,
            ..fake(Some(PhysicalSize { w: 3840, h: 2160 }), 1.0)
        };
        let saved = geometry_to_persist(&ws, prior, false).unwrap();
        assert!(saved.maximized);

        // Next boot off that saved state comes up maximized at the prior size.
        let boot = resolve_boot(saved, Scale(1.0), identity_clamp);
        assert!(boot.maximized);
        assert_eq!(boot.logical, LogicalSize { w: 1280, h: 720 });
    }
}
