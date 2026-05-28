//! CLI-driven Wayland paint-mode override. Set once by `app.rs` before
//! `early_init`; read by `lifecycle::jfn_wl_lifecycle_init` to bypass
//! the EGL/GBM dmabuf probe.

use std::sync::OnceLock;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WlPaintOverride {
    /// Force the EGL/GBM dmabuf shared-texture path. Leaves
    /// `shared_texture` enabled without probing.
    Dmabuf,
    /// Force the Vulkan-WSI pixel-upload path via `jfn_gpu_paint`.
    /// Calls `set_shared_texture_unsupported` (CEF emits BGRA) and
    /// presents through `vkCreateWaylandSurfaceKHR` rather than
    /// `wl_shm`. Hard-fails init if no Vulkan adapter is usable.
    Gpu,
    /// Force the `wl_shm` CPU path. Calls
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
