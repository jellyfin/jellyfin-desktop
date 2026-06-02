//! CLI-driven X11 paint preference. Set once by `app.rs` before
//! `early_init`; read by `lifecycle::init` to choose the entry tier of
//! the dmabuf → gpu → shm fallback chain.

use std::sync::OnceLock;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum X11PaintOverride {
    /// Prefer the zero-copy dmabuf-import path (CEF `OnAcceleratedPaint`
    /// → Vulkan external memory). Falls back to the Vulkan pixel-upload
    /// tier when the device can't import dmabufs.
    Dmabuf,
    /// Prefer the Vulkan pixel-upload path; falls back to shm if no usable
    /// adapter is available.
    Gpu,
    /// Prefer the MIT-SHM CPU path. Skips Vulkan init entirely.
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
