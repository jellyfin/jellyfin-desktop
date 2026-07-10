//! Stored as ONE `Option<WindowExtent>` swapped under a lock. Readers that need
//! several fields coherently take a single [`window_extent`] snapshot; the
//! per-field accessors read one field each and must not be composed into a
//! geometry that spans two generations.

use std::ffi::c_int;
use std::num::NonZeroU32;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use parking_lot::RwLock;

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

/// wp_fractional_scale reports scale in 120ths (120 = 1.0).
const SCALE_120_BASE: u32 = 120;

/// Sentinel for the `SCALE_120` atomic: no scale reported yet.
const SCALE_120_UNKNOWN: u32 = 0;

fn scale_ratio(scale_120: NonZeroU32) -> f32 {
    scale_120.get() as f32 / SCALE_120_BASE as f32
}

#[derive(Clone, Copy)]
pub(crate) struct WindowExtent {
    logical: WindowSize,
    physical: WindowSize,
    scale_120: NonZeroU32,
    generation: u64,
    mode: WindowMode,
}

impl WindowExtent {
    fn build(
        logical: WindowSize,
        scale_120: NonZeroU32,
        mode: WindowMode,
        generation: u64,
    ) -> Option<Self> {
        let base = i64::from(SCALE_120_BASE);
        let s = i64::from(scale_120.get());
        let physical = WindowSize::new(
            ((i64::from(logical.w) * s + base / 2) / base) as c_int,
            ((i64::from(logical.h) * s + base / 2) / base) as c_int,
        )?;
        Some(Self {
            logical,
            physical,
            scale_120,
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
            scale: scale_ratio(e.scale_120),
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

// Transitional shims for the per-field WindowSource; removed once it takes
// one WindowExtent snapshot.
pub(crate) fn jfn_wl_window_size_known() -> bool {
    extent().is_some()
}

pub(crate) fn jfn_wl_window_size() -> (c_int, c_int) {
    extent().map_or((0, 0), |e| (e.physical.w, e.physical.h))
}

pub(crate) fn jfn_wl_scale_known() -> bool {
    NonZeroU32::new(SCALE_120.load(Ordering::Acquire)).is_some()
}

pub(crate) fn jfn_wl_get_cached_scale() -> f32 {
    extent()
        .map(|e| e.scale_120)
        .or_else(|| NonZeroU32::new(SCALE_120.load(Ordering::Acquire)))
        .map_or(1.0, scale_ratio)
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
    let Some(scale_120) = NonZeroU32::new(SCALE_120.load(Ordering::Acquire)) else {
        return;
    };
    let generation = GENERATION.fetch_add(1, Ordering::AcqRel) + 1;
    let Some(extent) = WindowExtent::build(logical, scale_120, mode, generation) else {
        return;
    };
    *WINDOW_EXTENT.write() = Some(extent);
    tracing::debug!(
        target: "Main",
        "window extent gen={} logical={}x{} physical={}x{} scale={}",
        extent.generation, extent.logical.w, extent.logical.h, extent.physical.w, extent.physical.h, scale_120
    );

    let fullscreen = mode == WindowMode::Fullscreen;
    crate::wl_ffi::sync_maximized_command_state(mode == WindowMode::Maximized);
    if crate::wl_state::try_state().is_some() {
        wl_ops::on_configure(fullscreen);
    }
    let scale = scale_ratio(extent.scale_120);
    jfn_playback_post_osd_pixels(extent.physical.w, extent.physical.h, scale, false, 0, 0);
    // Wake any thread parked in `mpv_wait_event` (the boot-time VO-wait loop
    // reads OSD pixels from the ingest layer rather than via an mpv event).
    jfn_mpv::api::jfn_mpv_wakeup();
}

/// Satisfy the boot scale gate when no `wp_fractional_scale_manager_v1` exists,
/// so it doesn't wait forever for a `preferred_scale` that never arrives.
pub(crate) fn feed_unit_scale() {
    feed_scale(SCALE_120_BASE as c_int);
}

pub(crate) fn feed_scale(scale_120: c_int) {
    let Some(scale_120) = u32::try_from(scale_120).ok().and_then(NonZeroU32::new) else {
        return;
    };
    let first = SCALE_120.swap(scale_120.get(), Ordering::AcqRel) == SCALE_120_UNKNOWN;
    if first {
        tracing::info!(target: "Main", "scale known: {}", scale_ratio(scale_120));
    }
    jfn_mpv::api::jfn_mpv_wakeup();
}

pub(crate) fn feed_suspended(suspended: bool) {
    jfn_playback::lifecycle::jfn_lifecycle_set_visible(!suspended);
}
