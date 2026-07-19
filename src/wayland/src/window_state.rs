//! The single owner of Wayland window geometry/scale state. Everything lives
//! in ONE `RwLock<State>`: the last fed scale (with its provenance) and the
//! last published extent. Readers that need several fields coherently take a
//! single [`window_extent`] snapshot; the per-field accessors read one field
//! each and must not be composed into a geometry that spans two generations.

use parking_lot::RwLock;

use crate::scale::Scale120;
use crate::wl_ops;

use jfn_playback::ingest_driver::jfn_playback_post_osd_pixels;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct WindowSize {
    w: i32,
    h: i32,
}

impl WindowSize {
    pub(crate) fn new(w: i32, h: i32) -> Option<Self> {
        (w > 0 && h > 0).then_some(Self { w, h })
    }

    pub(crate) fn w(self) -> i32 {
        self.w
    }

    pub(crate) fn h(self) -> i32 {
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

/// Where the current scale came from. A provisional scale (output probe, or
/// the unit fallback when the compositor offers no fractional-scale protocol)
/// is a stand-in until the compositor's authoritative `preferred_scale`
/// arrives; an authoritative scale is never displaced by a provisional one.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ScaleProvenance {
    Provisional,
    Authoritative,
}

#[derive(Clone, Copy)]
struct KnownScale {
    scale: Scale120,
    provenance: ScaleProvenance,
}

#[derive(Clone, Copy)]
struct WindowExtent {
    logical: WindowSize,
    physical: WindowSize,
    scale: KnownScale,
    generation: u64,
    mode: WindowMode,
}

impl WindowExtent {
    fn build(
        logical: WindowSize,
        scale: KnownScale,
        mode: WindowMode,
        generation: u64,
    ) -> Option<Self> {
        let physical = scale.scale.physical_size(logical)?;
        Some(Self {
            logical,
            physical,
            scale,
            generation,
            mode,
        })
    }
}

struct State {
    scale: Option<KnownScale>,
    extent: Option<WindowExtent>,
    generation: u64,
}

static STATE: RwLock<State> = RwLock::new(State {
    scale: None,
    extent: None,
    generation: 0,
});

fn extent() -> Option<WindowExtent> {
    STATE.read().extent
}

/// A coherent view of the window geometry from one lock acquisition.
#[derive(Clone, Copy)]
pub(crate) struct WindowExtentSnapshot {
    logical: WindowSize,
    physical: WindowSize,
    scale: f32,
    mode: WindowMode,
}

impl WindowExtentSnapshot {
    fn from_extent(e: &WindowExtent) -> Self {
        Self {
            logical: e.logical,
            physical: e.physical,
            scale: e.scale.scale.ratio_f32(),
            mode: e.mode,
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

    pub(crate) fn mode(&self) -> WindowMode {
        self.mode
    }
}

pub(crate) fn window_extent() -> Option<WindowExtentSnapshot> {
    extent().map(|e| WindowExtentSnapshot::from_extent(&e))
}

pub(crate) fn window_logical_size() -> Option<WindowSize> {
    extent().map(|e| e.logical)
}

pub(crate) fn known_scale() -> Option<Scale120> {
    STATE.read().scale.map(|k| k.scale)
}

pub(crate) fn scale_known() -> bool {
    known_scale().is_some()
}

pub(crate) fn cached_scale() -> f32 {
    let st = STATE.read();
    st.extent
        .map(|e| e.scale.scale)
        .or(st.scale.map(|k| k.scale))
        .map_or(1.0, Scale120::ratio_f32)
}

pub(crate) fn window_maximized() -> bool {
    matches!(extent().map(|e| e.mode), Some(WindowMode::Maximized))
}

/// The consumer notifications below read the value back through the accessors,
/// so they must run after the write lock is released or they deadlock.
pub(crate) fn publish(logical: WindowSize, mode: WindowMode) {
    let Some(extent) = ({
        let mut st = STATE.write();
        let Some(scale) = st.scale else {
            return;
        };
        st.generation += 1;
        let extent = WindowExtent::build(logical, scale, mode, st.generation);
        if let Some(e) = extent {
            st.extent = Some(e);
        }
        extent
    }) else {
        return;
    };
    tracing::debug!(
        target: "Main",
        "window extent gen={} logical={}x{} physical={}x{} scale={}",
        extent.generation, extent.logical.w, extent.logical.h, extent.physical.w, extent.physical.h,
        extent.scale.scale
    );

    let fullscreen = mode == WindowMode::Fullscreen;
    crate::root_window::sync_maximized_command_state(mode == WindowMode::Maximized);
    if crate::wl_state::try_state().is_some() {
        wl_ops::on_configure(fullscreen);
    }
    let scale = extent.scale.scale.ratio_f32();
    jfn_playback_post_osd_pixels(extent.physical.w, extent.physical.h, scale, false, 0, 0);
    // mpv's `fullscreen` / `window-maximized` observations are skipped on
    // Wayland (the compositor owns this toplevel); this is the only mode feed.
    jfn_playback::ingest_driver::jfn_playback_post_window_state(
        mode == WindowMode::Fullscreen,
        mode == WindowMode::Maximized,
    );
    // Wake any thread parked in `mpv_wait_event` (the boot-time VO-wait loop
    // reads OSD pixels from the ingest layer rather than via an mpv event).
    jfn_mpv::api::jfn_mpv_wakeup();
}

/// Satisfy the boot scale gate when no `wp_fractional_scale_manager_v1` exists,
/// so it doesn't wait forever for a `preferred_scale` that never arrives.
pub(crate) fn feed_unit_scale() {
    feed_scale(Scale120::UNIT, ScaleProvenance::Provisional);
}

/// Pure arbitration: an authoritative scale always wins; a provisional one
/// never displaces an authoritative one (a late probe result must not clobber
/// the compositor's `preferred_scale`).
pub(crate) fn scale_displaces(current: Option<ScaleProvenance>, incoming: ScaleProvenance) -> bool {
    match (current, incoming) {
        (None, _) | (Some(_), ScaleProvenance::Authoritative) => true,
        (Some(cur), ScaleProvenance::Provisional) => cur == ScaleProvenance::Provisional,
    }
}

/// Record a scale, subject to [`scale_displaces`].
pub(crate) fn feed_scale(scale: Scale120, provenance: ScaleProvenance) {
    let first = {
        let mut st = STATE.write();
        let first = st.scale.is_none();
        if !scale_displaces(st.scale.map(|k| k.provenance), provenance) {
            return;
        }
        st.scale = Some(KnownScale { scale, provenance });
        first
    };
    if first {
        tracing::info!(target: "Main", "scale known: {scale}");
    }
    jfn_mpv::api::jfn_mpv_wakeup();
}

pub(crate) fn feed_suspended(suspended: bool) {
    jfn_playback::lifecycle::jfn_lifecycle_set_visible(!suspended);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn any_scale_fills_an_empty_slot() {
        assert!(scale_displaces(None, ScaleProvenance::Provisional));
        assert!(scale_displaces(None, ScaleProvenance::Authoritative));
    }

    #[test]
    fn authoritative_always_displaces() {
        assert!(scale_displaces(
            Some(ScaleProvenance::Provisional),
            ScaleProvenance::Authoritative
        ));
        assert!(scale_displaces(
            Some(ScaleProvenance::Authoritative),
            ScaleProvenance::Authoritative
        ));
    }

    #[test]
    fn provisional_corrects_provisional_but_never_authoritative() {
        assert!(scale_displaces(
            Some(ScaleProvenance::Provisional),
            ScaleProvenance::Provisional
        ));
        assert!(!scale_displaces(
            Some(ScaleProvenance::Authoritative),
            ScaleProvenance::Provisional
        ));
    }
}
