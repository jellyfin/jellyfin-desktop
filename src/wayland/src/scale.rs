//! Fractional window scale in 120ths, the unit of `wp_fractional_scale_v1`
//! (120 = 1.0). [`Scale120`] owns protocol parsing, ratio conversion, and
//! checked dimension scaling, so a zero/negative/non-finite scale or an
//! unrepresentable physical extent cannot leave this module.

use std::fmt;
use std::num::NonZeroU32;

use crate::window_state::WindowSize;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) struct Scale120(NonZeroU32);

impl Scale120 {
    /// wp_fractional_scale reports scale in 120ths (120 = 1.0).
    pub(crate) const BASE: u32 = 120;

    /// 1.0.
    pub(crate) const UNIT: Self = match NonZeroU32::new(Self::BASE) {
        Some(s) => Self(s),
        None => unreachable!(),
    };

    /// Parse a `wp_fractional_scale_v1.preferred_scale` wire value (120ths;
    /// zero is invalid on the wire).
    pub(crate) fn from_wire(raw: u32) -> Option<Self> {
        NonZeroU32::new(raw).map(Self)
    }

    /// Exact rational physical/logical width, rounded to the nearest 120th —
    /// no float round-trip.
    pub(crate) fn from_physical_logical(physical: u32, logical: NonZeroU32) -> Option<Self> {
        let num = u64::from(physical).checked_mul(u64::from(Self::BASE))?;
        let den = u64::from(logical.get());
        let scaled = (num + den / 2) / den;
        Self::from_wire(u32::try_from(scaled).ok()?)
    }

    pub(crate) fn ratio_f32(self) -> f32 {
        self.0.get() as f32 / Self::BASE as f32
    }

    /// Scale one logical dimension to physical (round half up), or `None` when
    /// the result cannot be represented as a positive `i32`.
    fn scale_dim(self, logical: i32) -> Option<i32> {
        let base = i64::from(Self::BASE);
        let scaled = i64::from(logical)
            .checked_mul(i64::from(self.0.get()))?
            .checked_add(base / 2)?
            / base;
        i32::try_from(scaled).ok()
    }

    /// Physical size for a logical size, or `None` when either dimension is
    /// unrepresentable.
    pub(crate) fn physical_size(self, logical: WindowSize) -> Option<WindowSize> {
        WindowSize::new(self.scale_dim(logical.w())?, self.scale_dim(logical.h())?)
    }
}

impl fmt::Display for Scale120 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", f64::from(self.0.get()) / f64::from(Self::BASE))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wire_zero_rejected() {
        assert_eq!(Scale120::from_wire(0), None);
    }

    #[test]
    fn wire_roundtrip() {
        let s = Scale120::from_wire(150).unwrap();
        assert_eq!(s.ratio_f32(), 1.25);
    }

    #[test]
    fn rational_matches_exact_ratios() {
        let logical = NonZeroU32::new(1920).unwrap();
        assert_eq!(
            Scale120::from_physical_logical(1920, logical),
            Some(Scale120::UNIT)
        );
        assert_eq!(
            Scale120::from_physical_logical(2400, logical),
            Scale120::from_wire(150)
        );
        assert_eq!(
            Scale120::from_physical_logical(2880, logical),
            Scale120::from_wire(180)
        );
    }

    #[test]
    fn rational_rounds_half_up_and_rejects_zero() {
        // physical 0 → scale 0 → rejected.
        assert_eq!(
            Scale120::from_physical_logical(0, NonZeroU32::new(1).unwrap()),
            None
        );
        // 1 physical / 240 logical = 0.5 in 120ths → rounds up to 1.
        assert_eq!(
            Scale120::from_physical_logical(1, NonZeroU32::new(240).unwrap()),
            Scale120::from_wire(1)
        );
    }

    #[test]
    fn rational_rejects_overflowing_result() {
        assert_eq!(
            Scale120::from_physical_logical(u32::MAX, NonZeroU32::new(1).unwrap()),
            None
        );
    }

    #[test]
    fn physical_size_rounds_half_up() {
        let s = Scale120::from_wire(150).unwrap(); // 1.25
        let logical = WindowSize::new(1280, 721).unwrap();
        let physical = s.physical_size(logical).unwrap();
        assert_eq!(physical.w(), 1600);
        // 721 * 1.25 = 901.25 → 901.
        assert_eq!(physical.h(), 901);
        let s = Scale120::from_wire(180).unwrap(); // 1.5
        let physical = s.physical_size(WindowSize::new(1, 1).unwrap()).unwrap();
        // 1.5 rounds half up to 2.
        assert_eq!(physical.w(), 2);
    }

    #[test]
    fn physical_size_rejects_dimension_overflow() {
        let s = Scale120::from_wire(240).unwrap(); // 2.0
        let logical = WindowSize::new(i32::MAX, 100).unwrap();
        assert_eq!(s.physical_size(logical), None);
    }

    #[test]
    fn physical_size_survives_extreme_scale_times_extreme_dim() {
        // i32::MAX * u32::MAX in 120ths overflows i64 mid-computation without
        // checked arithmetic.
        let s = Scale120::from_wire(u32::MAX).unwrap();
        let logical = WindowSize::new(i32::MAX, i32::MAX).unwrap();
        assert_eq!(s.physical_size(logical), None);
    }

    #[test]
    fn unit_scale_is_identity() {
        let logical = WindowSize::new(1280, 720).unwrap();
        assert_eq!(Scale120::UNIT.physical_size(logical), Some(logical));
    }

    #[test]
    fn display_formats_as_ratio() {
        assert_eq!(Scale120::UNIT.to_string(), "1");
        assert_eq!(Scale120::from_wire(150).unwrap().to_string(), "1.25");
    }

    // ------------------------------------------------------------------
    // Property tests over deterministic seeded samples (no proptest dep):
    // the checked arithmetic must agree with an exact wide-integer oracle
    // on every input, including extremes.
    // ------------------------------------------------------------------

    fn lcg(seed: u64) -> impl FnMut() -> u64 {
        let mut s = seed;
        move || {
            s = s
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            s
        }
    }

    const WIRE_EDGES: [u32; 6] = [1, 119, 120, 121, 240, u32::MAX];
    const DIM_EDGES: [i32; 5] = [1, 2, 120, i32::MAX - 1, i32::MAX];

    #[test]
    fn physical_size_agrees_with_exact_rational_oracle() {
        let mut next = lcg(0x5CA1E);
        let check = |wire: u32, w: i32, h: i32| {
            let (Some(scale), Some(logical)) = (Scale120::from_wire(wire), WindowSize::new(w, h))
            else {
                return;
            };
            let oracle = |d: i32| (i128::from(d) * i128::from(wire) + 60) / 120;
            let (ow, oh) = (oracle(w), oracle(h));
            let representable = |d: i128| (1..=i128::from(i32::MAX)).contains(&d);
            match scale.physical_size(logical) {
                Some(p) => {
                    assert_eq!(i128::from(p.w()), ow, "wire={wire} w={w}");
                    assert_eq!(i128::from(p.h()), oh, "wire={wire} h={h}");
                }
                None => {
                    assert!(
                        !representable(ow) || !representable(oh),
                        "wire={wire} {w}x{h} rejected but representable"
                    );
                }
            }
        };
        for wire in WIRE_EDGES {
            for w in DIM_EDGES {
                for h in DIM_EDGES {
                    check(wire, w, h);
                }
            }
        }
        for _ in 0..10_000 {
            let wire = (next() % 1200 + 1) as u32;
            let w = (next() % 20_000 + 1) as i32;
            let h = (next() % 20_000 + 1) as i32;
            check(wire, w, h);
        }
    }

    #[test]
    fn from_physical_logical_agrees_with_exact_rational_oracle() {
        let mut next = lcg(0xF00D);
        let check = |physical: u32, logical: u32| {
            let Some(logical_nz) = NonZeroU32::new(logical) else {
                return;
            };
            let oracle =
                (u128::from(physical) * 120 + u128::from(logical) / 2) / u128::from(logical);
            match Scale120::from_physical_logical(physical, logical_nz) {
                Some(s) => assert_eq!(
                    Scale120::from_wire(u32::try_from(oracle).unwrap()),
                    Some(s),
                    "{physical}/{logical}"
                ),
                None => assert!(
                    oracle == 0 || oracle > u128::from(u32::MAX),
                    "{physical}/{logical} rejected but oracle={oracle}"
                ),
            }
        };
        for physical in [0u32, 1, 119, 120, 1920, 3840, u32::MAX] {
            for logical in [1u32, 2, 120, 1920, u32::MAX] {
                check(physical, logical);
            }
        }
        for _ in 0..10_000 {
            let physical = (next() % 20_000) as u32;
            let logical = (next() % 20_000 + 1) as u32;
            check(physical, logical);
        }
    }

    #[test]
    fn scale_then_rederive_roundtrips_within_one_120th() {
        let mut next = lcg(0xB0BA);
        for _ in 0..10_000 {
            // Realistic display range: scales 0.5..=4.0, widths ≥ 120.
            let wire = (next() % 421 + 60) as u32;
            let w = (next() % 7_500 + 120) as i32;
            let scale = Scale120::from_wire(wire).unwrap();
            let logical = WindowSize::new(w, w).unwrap();
            let Some(physical) = scale.physical_size(logical) else {
                continue;
            };
            let rederived = Scale120::from_physical_logical(
                u32::try_from(physical.w()).unwrap(),
                NonZeroU32::new(u32::try_from(w).unwrap()).unwrap(),
            )
            .unwrap();
            // Rounding the physical size loses at most half a pixel, which for
            // widths ≥ 120 is at most one 120th of scale.
            assert!(
                [wire - 1, wire, wire + 1]
                    .into_iter()
                    .any(|cand| Scale120::from_wire(cand) == Some(rederived)),
                "wire={wire} w={w} rederived out of tolerance"
            );
        }
    }
}
