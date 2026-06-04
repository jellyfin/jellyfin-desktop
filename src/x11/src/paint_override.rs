//! CLI-driven X11 paint preference. Must be set before `early_init`, since the
//! backing `OnceLock` ignores later writes.

use std::sync::OnceLock;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum X11PaintOverride {
    Dmabuf,
    Gpu,
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
