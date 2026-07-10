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

/// Physical size paired with its display scale. Reading the two coherently is
/// the producer's responsibility, not this type's: the Wayland source derives
/// both from one window-extent snapshot, while the mpv-backed source reads
/// ingest size and `Platform::get_scale` separately because it has no bundled
/// snapshot to offer. `WindowExtent` only carries the pair it is handed.
#[derive(Copy, Clone, Debug, PartialEq)]
pub struct WindowExtent {
    physical: PhysicalSize,
    scale: Scale,
}

impl WindowExtent {
    pub fn new(physical: PhysicalSize, scale: Scale) -> Self {
        Self { physical, scale }
    }

    pub fn physical(&self) -> PhysicalSize {
        self.physical
    }

    pub fn scale(&self) -> Scale {
        self.scale
    }
}

/// Fully-resolved boot geometry: one typed value computed once from saved
/// config, consumed by `Platform::apply_boot_geometry` (logical) and mpv's
/// `--geometry` (physical).
#[derive(Copy, Clone, Debug, PartialEq)]
pub struct BootGeometry {
    logical: LogicalSize,
    physical: PhysicalSize,
    scale: Scale,
    /// `None` ⇒ let the window center (Wayland ignores position entirely).
    position: Option<WindowPos>,
    maximized: bool,
}

impl BootGeometry {
    /// The one constructor: `physical` and `position` are both taken from a
    /// single already-clamped [`WindowGeometry`], so they cannot disagree with
    /// each other or be set independently of the clamp. `scale` is the factor
    /// that produced `clamped` from `logical`.
    pub fn from_clamped(
        logical: LogicalSize,
        scale: Scale,
        clamped: WindowGeometry,
        maximized: bool,
    ) -> Self {
        Self {
            logical,
            physical: PhysicalSize {
                w: clamped.w,
                h: clamped.h,
            },
            scale,
            position: clamped.position,
            maximized,
        }
    }

    pub fn logical(&self) -> LogicalSize {
        self.logical
    }

    pub fn physical(&self) -> PhysicalSize {
        self.physical
    }

    pub fn scale(&self) -> Scale {
        self.scale
    }

    pub fn position(&self) -> Option<WindowPos> {
        self.position
    }

    pub fn maximized(&self) -> bool {
        self.maximized
    }

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

/// Saved window geometry: size plus an optional top-left position. `None`
/// position asks [`clamp_to_bounds`] to center the window (mpv's own centering
/// misbehaves when only the width/height are overridden).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct WindowGeometry {
    pub w: c_int,
    pub h: c_int,
    pub position: Option<WindowPos>,
}

impl WindowGeometry {
    /// Build from raw coordinates where a negative `x` or `y` means "unset".
    /// The single home for that OS/config-facing sentinel convention.
    pub fn from_raw(w: c_int, h: c_int, x: c_int, y: c_int) -> Self {
        Self {
            w,
            h,
            position: (x >= 0 && y >= 0).then_some(WindowPos { x, y }),
        }
    }

    /// Raw coordinates for OS APIs that take a sentinel; `(-1, -1)` when unset.
    pub fn raw_position(&self) -> (c_int, c_int) {
        self.position.map_or((-1, -1), |p| (p.x, p.y))
    }
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
    // Center an unset position; otherwise start from the requested one.
    let (mut x, mut y) = match g.position {
        Some(p) => (p.x, p.y),
        None => ((vw - g.w) / 2, (vh - g.h) / 2),
    };
    if x + g.w > vw {
        x = vw - g.w;
    }
    if y + g.h > vh {
        y = vh - g.h;
    }
    if x < 0 {
        x = 0;
    }
    if y < 0 {
        y = 0;
    }
    g.position = Some(WindowPos { x, y });
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
        let logical = LogicalSize { w: 1280, h: 720 };
        let base = BootGeometry::from_clamped(
            logical,
            Scale(1.25),
            WindowGeometry::from_raw(1600, 900, -1, -1),
            false,
        );
        assert_eq!(base.mpv_geometry_string(), "1600x900");
        assert!(!base.force_position());

        let positioned = BootGeometry::from_clamped(
            logical,
            Scale(1.25),
            WindowGeometry::from_raw(1600, 900, 100, 50),
            false,
        );
        assert_eq!(positioned.mpv_geometry_string(), "1600x900+100+50");
        assert!(positioned.force_position());
    }

    const SCREEN: Bounds = Bounds { w: 1920, h: 1080 };

    fn pos(g: &WindowGeometry) -> (c_int, c_int) {
        g.raw_position()
    }

    #[test]
    fn fits_unchanged() {
        let mut g = WindowGeometry::from_raw(800, 600, 100, 50);
        clamp_to_bounds(&mut g, SCREEN);
        assert_eq!(g, WindowGeometry::from_raw(800, 600, 100, 50));
    }

    #[test]
    fn oversized_shrinks_to_bounds() {
        let mut g = WindowGeometry::from_raw(3000, 2000, 0, 0);
        clamp_to_bounds(&mut g, SCREEN);
        assert_eq!(g.w, 1920);
        assert_eq!(g.h, 1080);
    }

    #[test]
    fn unset_axes_center() {
        let mut g = WindowGeometry::from_raw(800, 600, -1, -1);
        clamp_to_bounds(&mut g, SCREEN);
        assert_eq!(pos(&g), ((1920 - 800) / 2, (1080 - 600) / 2));
    }

    #[test]
    fn past_edge_pulls_back() {
        let mut g = WindowGeometry::from_raw(800, 600, 1500, 900);
        clamp_to_bounds(&mut g, SCREEN);
        assert_eq!(pos(&g), (1920 - 800, 1080 - 600));
    }

    #[test]
    fn oversized_then_floored_at_origin() {
        // Oversized window: shrink to bounds, center (negative → 0 after
        // edge-adjust + floor).
        let mut g = WindowGeometry::from_raw(3000, 2000, -1, -1);
        clamp_to_bounds(&mut g, SCREEN);
        assert_eq!(g, WindowGeometry::from_raw(1920, 1080, 0, 0));
    }
}
