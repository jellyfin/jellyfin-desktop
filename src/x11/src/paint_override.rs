//! CLI-driven X11 paint-mode override. Set once by `app.rs` before
//! `early_init`; read by `lifecycle::init` to bypass the Vulkan probe.

use std::sync::OnceLock;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum X11PaintOverride {
    /// Force the Vulkan pixel-upload path. Hard-fails init if no
    /// usable adapter is available.
    Gpu,
    /// Force the MIT-SHM CPU path. Skips Vulkan init entirely.
    Shm,
}

static OVERRIDE: OnceLock<X11PaintOverride> = OnceLock::new();

/// Set the override. No-op if called twice.
pub fn set_paint_override(mode: X11PaintOverride) {
    let _ = OVERRIDE.set(mode);
}

pub fn paint_override() -> Option<X11PaintOverride> {
    OVERRIDE.get().copied()
}
