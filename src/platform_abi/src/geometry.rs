//! Window-geometry value types + on-screen clamping.
//!
//! The clamp algorithm was byte-identical in `macos_clamp_window_geometry`
//! and `win_clamp_window_geometry`; only the OS bounds query differed
//! (`NSScreen.visibleFrame * scale` vs `SPI_GETWORKAREA`). That query stays
//! platform-side and hands the resolved [`Bounds`] in, so the shared logic
//! is testable on any host.

use std::ffi::c_int;

/// HiDPI scale factor (physical pixels per logical pixel).
#[derive(Copy, Clone, Debug, PartialEq)]
pub struct Scale(pub f32);

impl Scale {
    /// Replace a non-positive (unknown) scale with 1.0.
    pub fn or_one(self) -> Self {
        if self.0 > 0.0 { self } else { Scale(1.0) }
    }
}

/// Window size in logical (DIP) pixels — the coordinate space the compositor
/// uses for the toplevel; the display scale maps it to physical pixels.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct LogicalSize {
    pub w: c_int,
    pub h: c_int,
}

/// Window size in physical (backing) pixels — what mpv's `--geometry` takes and
/// what gets persisted as `windowWidth/Height`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct PhysicalSize {
    pub w: c_int,
    pub h: c_int,
}

impl LogicalSize {
    pub fn to_physical(self, s: Scale) -> PhysicalSize {
        let s = s.or_one().0;
        PhysicalSize {
            w: (self.w as f32 * s).round() as c_int,
            h: (self.h as f32 * s).round() as c_int,
        }
    }
}

impl PhysicalSize {
    pub fn to_logical(self, s: Scale) -> LogicalSize {
        let s = s.or_one().0;
        LogicalSize {
            w: (self.w as f32 / s).round() as c_int,
            h: (self.h as f32 / s).round() as c_int,
        }
    }
}

/// Fully-resolved boot geometry: one typed value computed once from saved
/// config, consumed by `Platform::apply_boot_geometry` (logical) and mpv's
/// `--geometry` (physical).
#[derive(Copy, Clone, Debug, PartialEq)]
pub struct BootGeometry {
    pub logical: LogicalSize,
    pub physical: PhysicalSize,
    pub scale: Scale,
    /// `None` ⇒ let the window center (Wayland ignores position entirely).
    pub position: Option<WindowPos>,
    pub maximized: bool,
}

impl BootGeometry {
    /// mpv `--geometry`: `"<W>x<H>"` or `"<W>x<H>+<X>+<Y>"`, physical pixels.
    pub fn mpv_geometry_string(&self) -> String {
        let mut s = format!("{}x{}", self.physical.w, self.physical.h);
        if let Some(p) = self.position {
            s.push_str(&format!("+{}+{}", p.x, p.y));
        }
        s
    }

    pub fn force_position(&self) -> bool {
        self.position.is_some()
    }
}

/// Working-area dimensions — excludes the menu bar / dock / taskbar — in the
/// same pixel space (backing pixels) as the geometry being clamped.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Bounds {
    pub w: c_int,
    pub h: c_int,
}

/// Saved window geometry: size plus top-left position. A negative `x`/`y`
/// means "unset" and asks [`clamp_to_bounds`] to center that axis (mpv's own
/// centering misbehaves when only the width/height are overridden).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct WindowGeometry {
    pub w: c_int,
    pub h: c_int,
    pub x: c_int,
    pub y: c_int,
}

/// A window's top-left position, in the coordinate space the backend
/// reports (backing pixels relative to the working area). Returned by
/// `Platform::query_window_position`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct WindowPos {
    pub x: c_int,
    pub y: c_int,
}

/// A surface resize request: logical (DIP) and physical (pixel) dimensions.
/// Carried as one struct through `Platform::surface_resize` so adding a
/// field later doesn't change the method's arity (and so doesn't churn
/// every backend + call site).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct SurfaceSize {
    pub logical_w: c_int,
    pub logical_h: c_int,
    pub physical_w: c_int,
    pub physical_h: c_int,
}

/// Clamp `g` so the window stays fully within `bounds`: shrink oversized
/// dimensions, center any unset (negative) axis, pull a past-the-edge window
/// back in-bounds, then floor at the origin. Byte-for-byte the former
/// per-platform clamp.
pub fn clamp_to_bounds(g: &mut WindowGeometry, bounds: Bounds) {
    let vw = bounds.w;
    let vh = bounds.h;
    if g.w > vw {
        g.w = vw;
    }
    if g.h > vh {
        g.h = vh;
    }
    if g.x < 0 {
        g.x = (vw - g.w) / 2;
    }
    if g.y < 0 {
        g.y = (vh - g.h) / 2;
    }
    if g.x + g.w > vw {
        g.x = vw - g.w;
    }
    if g.y + g.h > vh {
        g.y = vh - g.h;
    }
    if g.x < 0 {
        g.x = 0;
    }
    if g.y < 0 {
        g.y = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn logical_physical_round_trip() {
        for (logical, scale, physical) in [
            (
                LogicalSize { w: 1280, h: 720 },
                1.0,
                PhysicalSize { w: 1280, h: 720 },
            ),
            (
                LogicalSize { w: 1280, h: 720 },
                1.25,
                PhysicalSize { w: 1600, h: 900 },
            ),
            (
                LogicalSize { w: 1600, h: 900 },
                1.5,
                PhysicalSize { w: 2400, h: 1350 },
            ),
            (
                LogicalSize { w: 1280, h: 720 },
                2.0,
                PhysicalSize { w: 2560, h: 1440 },
            ),
        ] {
            assert_eq!(logical.to_physical(Scale(scale)), physical);
            assert_eq!(physical.to_logical(Scale(scale)), logical);
        }
    }

    #[test]
    fn scale_or_one_guards_nonpositive() {
        assert_eq!(Scale(0.0).or_one(), Scale(1.0));
        assert_eq!(Scale(-2.0).or_one(), Scale(1.0));
        assert_eq!(Scale(1.5).or_one(), Scale(1.5));
        assert_eq!(
            LogicalSize { w: 800, h: 600 }.to_physical(Scale(0.0)),
            PhysicalSize { w: 800, h: 600 }
        );
    }

    #[test]
    fn mpv_geometry_string_with_and_without_position() {
        let base = BootGeometry {
            logical: LogicalSize { w: 1280, h: 720 },
            physical: PhysicalSize { w: 1600, h: 900 },
            scale: Scale(1.25),
            position: None,
            maximized: false,
        };
        assert_eq!(base.mpv_geometry_string(), "1600x900");
        assert!(!base.force_position());

        let positioned = BootGeometry {
            position: Some(WindowPos { x: 100, y: 50 }),
            ..base
        };
        assert_eq!(positioned.mpv_geometry_string(), "1600x900+100+50");
        assert!(positioned.force_position());
    }

    const SCREEN: Bounds = Bounds { w: 1920, h: 1080 };

    #[test]
    fn fits_unchanged() {
        let mut g = WindowGeometry {
            w: 800,
            h: 600,
            x: 100,
            y: 50,
        };
        clamp_to_bounds(&mut g, SCREEN);
        assert_eq!(
            g,
            WindowGeometry {
                w: 800,
                h: 600,
                x: 100,
                y: 50
            }
        );
    }

    #[test]
    fn oversized_shrinks_to_bounds() {
        let mut g = WindowGeometry {
            w: 3000,
            h: 2000,
            x: 0,
            y: 0,
        };
        clamp_to_bounds(&mut g, SCREEN);
        assert_eq!(g.w, 1920);
        assert_eq!(g.h, 1080);
    }

    #[test]
    fn unset_axes_center() {
        let mut g = WindowGeometry {
            w: 800,
            h: 600,
            x: -1,
            y: -1,
        };
        clamp_to_bounds(&mut g, SCREEN);
        assert_eq!(g.x, (1920 - 800) / 2);
        assert_eq!(g.y, (1080 - 600) / 2);
    }

    #[test]
    fn past_edge_pulls_back() {
        let mut g = WindowGeometry {
            w: 800,
            h: 600,
            x: 1500,
            y: 900,
        };
        clamp_to_bounds(&mut g, SCREEN);
        assert_eq!(g.x, 1920 - 800);
        assert_eq!(g.y, 1080 - 600);
    }

    #[test]
    fn oversized_then_floored_at_origin() {
        // Oversized window: shrink to bounds, center (negative → 0 after
        // edge-adjust + floor).
        let mut g = WindowGeometry {
            w: 3000,
            h: 2000,
            x: -1,
            y: -1,
        };
        clamp_to_bounds(&mut g, SCREEN);
        assert_eq!(
            g,
            WindowGeometry {
                w: 1920,
                h: 1080,
                x: 0,
                y: 0
            }
        );
    }
}
