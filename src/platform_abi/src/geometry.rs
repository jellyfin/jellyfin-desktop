//! Window-geometry value types + on-screen clamping.
//!
//! The clamp algorithm was byte-identical in `macos_clamp_window_geometry`
//! and `win_clamp_window_geometry`; only the OS bounds query differed
//! (`NSScreen.visibleFrame * scale` vs `SPI_GETWORKAREA`). That query stays
//! platform-side and hands the resolved [`Bounds`] in, so the shared logic
//! is testable on any host.

use std::ffi::c_int;

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
