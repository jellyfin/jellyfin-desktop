//! Live window geometry, sourced from whichever component owns the window.

use crate::geometry::{WindowExtent, WindowPos};

#[derive(Clone, Copy)]
pub struct WindowSnapshot {
    pub extent: Option<WindowExtent>,
    pub position: Option<WindowPos>,
    pub maximized: bool,
    pub fullscreen: bool,
}

pub trait WindowSource: Send + Sync {
    fn snapshot(&self) -> WindowSnapshot;
}
