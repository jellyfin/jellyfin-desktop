//! Shared native context-menu core: the platform-agnostic layout/rasterization
//! (tiny-skia + cosmic-text) and the modal interaction state machine. The X11
//! and Wayland backends each drive these with their own windowing + input
//! plumbing.

#![cfg(target_os = "linux")]

pub mod interaction_fsm;
pub mod render;

pub use render::{Fonts, Layout, layout, paint};

/// One context-menu row. A separator has `separator = true`; its other fields
/// are ignored.
pub struct MenuItem {
    pub id: i32,
    pub label: String,
    pub enabled: bool,
    pub separator: bool,
}
