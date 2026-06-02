//! CLI-driven Wayland paint preference. Set once by `app.rs` before
//! `early_init`; read by `lifecycle::jfn_wl_lifecycle_init` to choose the
//! entry tier of the dmabuf → gpu → shm fallback chain. The probe still
//! runs — the preference only picks where the chain starts.

use std::sync::OnceLock;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WlPaintOverride {
    /// Prefer the EGL/GBM dmabuf shared-texture path. The probe still
    /// runs; on failure it falls back to gpu then shm.
    Dmabuf,
    /// Prefer the Vulkan-WSI pixel-upload path via `jfn_gpu_paint`.
    /// Calls `set_shared_texture_unsupported` (CEF emits BGRA) and
    /// presents through `vkCreateWaylandSurfaceKHR` rather than
    /// `wl_shm`. Falls back to `wl_shm` if no Vulkan adapter is usable.
    Gpu,
    /// Prefer the `wl_shm` CPU path. Calls
    /// `set_shared_texture_unsupported` immediately.
    Shm,
}

static OVERRIDE: OnceLock<WlPaintOverride> = OnceLock::new();

pub fn set_paint_override(mode: WlPaintOverride) {
    let _ = OVERRIDE.set(mode);
}

pub fn paint_override() -> Option<WlPaintOverride> {
    OVERRIDE.get().copied()
}
