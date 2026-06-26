//! Live window geometry, sourced from whichever component owns the window.

use crate::geometry::{PhysicalSize, WindowPos};

pub trait WindowSource: Send + Sync {
    fn size(&self) -> Option<PhysicalSize>;
    fn maximized(&self) -> bool;
    fn fullscreen(&self) -> bool;
    fn position(&self) -> Option<WindowPos>;
    /// Display scale; non-positive means unknown (guard with [`crate::scale_or_one`]).
    fn scale(&self) -> f64;
}
