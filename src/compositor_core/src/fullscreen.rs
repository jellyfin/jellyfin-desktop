//! Fullscreen-transition bookkeeping flags.
//!
//! Both the macOS and Windows backends track the same handful of booleans
//! across a fullscreen toggle: whether the window was fullscreen, whether
//! it was minimized, and whether to restore the maximized state on exit.
//! The *orchestration* (when to begin/end a transition, the `IsZoomed` /
//! `IsIconic` / mpv calls) stays platform-side because it's interleaved
//! with OS state queries — only this flag bag is shared.

/// Fullscreen-transition flags. Mirrors the Windows `WinState` fields
/// (`was_fullscreen`, `was_minimized`, `restore_maximized_on_unfullscreen`)
/// and the macOS equivalents.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct FullscreenState {
    pub was_fullscreen: bool,
    pub was_minimized: bool,
    pub restore_maximized: bool,
}

impl FullscreenState {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            was_fullscreen: false,
            was_minimized: false,
            restore_maximized: false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_all_false() {
        let f = FullscreenState::new();
        assert!(!f.was_fullscreen);
        assert!(!f.was_minimized);
        assert!(!f.restore_maximized);
        assert_eq!(FullscreenState::default(), f);
    }
}
