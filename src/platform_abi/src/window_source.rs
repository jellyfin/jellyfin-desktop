//! Live window geometry, sourced from whichever component owns the window.

use crate::geometry::{PhysicalSize, Scale, WindowPos};

pub trait WindowSource: Send + Sync {
    fn size(&self) -> Option<PhysicalSize>;
    fn maximized(&self) -> bool;
    fn fullscreen(&self) -> bool;
    fn position(&self) -> Option<WindowPos>;
    fn scale(&self) -> Scale;
}
