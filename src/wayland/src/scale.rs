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

    /// Parse a scale ratio (1.0 = 100%), e.g. from the output probe.
    pub(crate) fn from_ratio(ratio: f64) -> Option<Self> {
        let scaled = (ratio * f64::from(Self::BASE)).round();
        if !(1.0..=f64::from(u32::MAX)).contains(&scaled) {
            return None;
        }
        Self::from_wire(scaled as u32)
    }

    pub(crate) fn get(self) -> NonZeroU32 {
        self.0
    }

    pub(crate) fn ratio_f32(self) -> f32 {
        self.0.get() as f32 / Self::BASE as f32
    }

    /// Scale one logical dimension to physical (round half up), or `None` when
    /// the result cannot be represented as a positive `c_int`.
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
        assert_eq!(s.get().get(), 150);
        assert_eq!(s.ratio_f32(), 1.25);
    }

    #[test]
    fn ratio_rejects_non_finite_and_non_positive() {
        for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, 0.0, -1.0, -0.5] {
            assert_eq!(Scale120::from_ratio(bad), None, "{bad}");
        }
    }

    #[test]
    fn ratio_rejects_below_one_120th() {
        // Rounds to 0 in 120ths.
        assert_eq!(Scale120::from_ratio(0.001), None);
    }

    #[test]
    fn ratio_rejects_above_u32_max() {
        assert_eq!(Scale120::from_ratio(f64::from(u32::MAX)), None);
        assert_eq!(Scale120::from_ratio(1e300), None);
    }

    #[test]
    fn ratio_rounds_to_nearest_120th() {
        assert_eq!(Scale120::from_ratio(1.0), Some(Scale120::UNIT));
        assert_eq!(Scale120::from_ratio(1.25).unwrap().get().get(), 150);
        // 1.3 * 120 = 156.0 exactly in this decimal; 1.301 rounds to 156 too.
        assert_eq!(Scale120::from_ratio(1.301).unwrap().get().get(), 156);
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
}
