//! Stored as ONE `Option<WindowExtent>` swapped under a lock. Readers that need
//! several fields coherently take a single [`window_extent`] snapshot; the
//! per-field accessors read one field each and must not be composed into a
//! geometry that spans two generations.

use std::ffi::c_int;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use parking_lot::RwLock;

use crate::scale::Scale120;
use crate::wl_ops;

use jfn_playback::ingest_driver::jfn_playback_post_osd_pixels;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct WindowSize {
    w: c_int,
    h: c_int,
}

impl WindowSize {
    pub(crate) fn new(w: c_int, h: c_int) -> Option<Self> {
        (w > 0 && h > 0).then_some(Self { w, h })
    }

    pub(crate) fn w(self) -> c_int {
        self.w
    }

    pub(crate) fn h(self) -> c_int {
        self.h
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum WindowMode {
    Floating,
    /// Compositor-tiled (snapped). Like Maximized/Fullscreen the size is
    /// compositor-dictated, so it must not feed the floating restore size.
    Tiled,
    Maximized,
    Fullscreen,
}

impl WindowMode {
    pub(crate) fn uses_floating_restore(self) -> bool {
        matches!(self, WindowMode::Floating)
    }
}

/// Sentinel for the `SCALE_120` atomic: no scale reported yet.
const SCALE_120_UNKNOWN: u32 = 0;

#[derive(Clone, Copy)]
pub(crate) struct WindowExtent {
    logical: WindowSize,
    physical: WindowSize,
    scale: Scale120,
    generation: u64,
    mode: WindowMode,
}

impl WindowExtent {
    fn build(
        logical: WindowSize,
        scale: Scale120,
        mode: WindowMode,
        generation: u64,
    ) -> Option<Self> {
        let physical = scale.physical_size(logical)?;
        Some(Self {
            logical,
            physical,
            scale,
            generation,
            mode,
        })
    }
}

static WINDOW_EXTENT: RwLock<Option<WindowExtent>> = RwLock::new(None);

static SCALE_120: AtomicU32 = AtomicU32::new(SCALE_120_UNKNOWN);

static GENERATION: AtomicU64 = AtomicU64::new(0);

fn extent() -> Option<WindowExtent> {
    *WINDOW_EXTENT.read()
}

/// A coherent view of the window geometry from one lock acquisition.
#[derive(Clone, Copy)]
pub(crate) struct WindowExtentSnapshot {
    logical: WindowSize,
    physical: WindowSize,
    scale: f32,
}

impl WindowExtentSnapshot {
    fn from_extent(e: &WindowExtent) -> Self {
        Self {
            logical: e.logical,
            physical: e.physical,
            scale: e.scale.ratio_f32(),
        }
    }

    pub(crate) fn logical(&self) -> WindowSize {
        self.logical
    }

    pub(crate) fn physical(&self) -> WindowSize {
        self.physical
    }

    pub(crate) fn scale(&self) -> f32 {
        self.scale
    }
}

pub(crate) fn window_extent() -> Option<WindowExtentSnapshot> {
    extent().map(|e| WindowExtentSnapshot::from_extent(&e))
}

pub(crate) fn window_logical_size() -> Option<WindowSize> {
    extent().map(|e| e.logical)
}

fn fed_scale() -> Option<Scale120> {
    Scale120::from_wire(SCALE_120.load(Ordering::Acquire))
}

pub(crate) fn jfn_wl_scale_known() -> bool {
    fed_scale().is_some()
}

pub(crate) fn jfn_wl_get_cached_scale() -> f32 {
    extent()
        .map(|e| e.scale)
        .or_else(fed_scale)
        .map_or(1.0, Scale120::ratio_f32)
}

pub(crate) fn jfn_wl_window_maximized() -> bool {
    matches!(extent().map(|e| e.mode), Some(WindowMode::Maximized))
}

pub(crate) fn jfn_wl_window_fullscreen() -> bool {
    matches!(extent().map(|e| e.mode), Some(WindowMode::Fullscreen))
}

/// The consumer notifications below read the value back through the accessors,
/// so they must run after the write lock is released or they deadlock.
pub(crate) fn publish(logical_w: c_int, logical_h: c_int, mode: WindowMode) {
    let Some(logical) = WindowSize::new(logical_w, logical_h) else {
        return;
    };
    let Some(scale) = fed_scale() else {
        return;
    };
    let generation = GENERATION.fetch_add(1, Ordering::AcqRel) + 1;
    let Some(extent) = WindowExtent::build(logical, scale, mode, generation) else {
        return;
    };
    *WINDOW_EXTENT.write() = Some(extent);
    tracing::debug!(
        target: "Main",
        "window extent gen={} logical={}x{} physical={}x{} scale={}",
        extent.generation, extent.logical.w, extent.logical.h, extent.physical.w, extent.physical.h, scale
    );

    let fullscreen = mode == WindowMode::Fullscreen;
    crate::wl_ffi::sync_maximized_command_state(mode == WindowMode::Maximized);
    if crate::wl_state::try_state().is_some() {
        wl_ops::on_configure(fullscreen);
    }
    let scale = extent.scale.ratio_f32();
    jfn_playback_post_osd_pixels(extent.physical.w, extent.physical.h, scale, false, 0, 0);
    // Wake any thread parked in `mpv_wait_event` (the boot-time VO-wait loop
    // reads OSD pixels from the ingest layer rather than via an mpv event).
    jfn_mpv::api::jfn_mpv_wakeup();
}

/// Satisfy the boot scale gate when no `wp_fractional_scale_manager_v1` exists,
/// so it doesn't wait forever for a `preferred_scale` that never arrives.
pub(crate) fn feed_unit_scale() {
    feed_scale(Scale120::UNIT);
}

pub(crate) fn feed_scale(scale: Scale120) {
    let first = SCALE_120.swap(scale.get().get(), Ordering::AcqRel) == SCALE_120_UNKNOWN;
    if first {
        tracing::info!(target: "Main", "scale known: {scale}");
    }
    jfn_mpv::api::jfn_mpv_wakeup();
}

pub(crate) fn feed_suspended(suspended: bool) {
    jfn_playback::lifecycle::jfn_lifecycle_set_visible(!suspended);
}
