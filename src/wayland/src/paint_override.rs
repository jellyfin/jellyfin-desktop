//! Must be set before `early_init`; `lifecycle::jfn_wl_lifecycle_init`
//! reads it once during init.

use std::sync::OnceLock;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WlPaintOverride {
    Dmabuf,
    Gpu,
    Shm,
}

static OVERRIDE: OnceLock<WlPaintOverride> = OnceLock::new();

pub fn set_paint_override(mode: WlPaintOverride) {
    let _ = OVERRIDE.set(mode);
}

pub fn paint_override() -> Option<WlPaintOverride> {
    OVERRIDE.get().copied()
}
