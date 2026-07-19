//! Live fractional-scale probe: open an own `wl_display` connection, read the
//! outputs' logical (xdg-output) and mode geometry, disconnect. Used before
//! the surface has entered any output — boot-time `--geometry` correction and
//! the first-configure fallback — so the output is chosen provisionally: by
//! containing point when one is given, else the first usable output.
//!
//! Output selection and scale calculation are pure ([`select_scale`]) over
//! [`OutputCandidate`]s; only [`probe_scale`] talks to the live display.

use std::env;
use std::num::NonZeroU32;

use smithay_client_toolkit::output::{OutputHandler, OutputState};
use smithay_client_toolkit::registry::{ProvidesRegistryState, RegistryState};
use smithay_client_toolkit::{delegate_output, delegate_registry, registry_handlers};
use wayland_client::globals::registry_queue_init;
use wayland_client::protocol::wl_output;
use wayland_client::{Connection, QueueHandle};

use crate::scale::Scale120;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ScaleProbeError {
    /// No `WAYLAND_DISPLAY`/`WAYLAND_SOCKET` in the environment.
    NoWaylandSession,
    /// Connecting or round-tripping on the probe connection failed.
    Connection,
    /// No output offered complete, positive geometry to derive a scale from.
    NoUsableOutput,
    /// The probe thread outlived its deadline (the compositor stalled the
    /// probe connection's round trips).
    Timeout,
}

impl std::fmt::Display for ScaleProbeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NoWaylandSession => write!(f, "no Wayland session"),
            Self::Connection => write!(f, "probe connection failed"),
            Self::NoUsableOutput => write!(f, "no usable output"),
            Self::Timeout => write!(f, "probe timed out"),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ProbeTarget {
    /// The output containing this point (compositor logical coordinates).
    Point { x: i32, y: i32 },
    /// The first usable output, for callers with no better anchor.
    FirstOutput,
}

/// One output's geometry as needed for scale derivation. Constructed only
/// from complete metadata — an output still mid-advertisement is skipped, not
/// guessed at.
#[derive(Clone, Copy, Debug)]
pub(crate) struct OutputCandidate {
    logical_pos: (i32, i32),
    logical_size: (i32, i32),
    /// Current mode dimensions, in the panel's native (untransformed) axes.
    mode: (i32, i32),
    /// Output transform rotates by 90/270°, so the mode's axes are swapped
    /// relative to the logical size.
    swaps_axes: bool,
}

impl OutputCandidate {
    pub(crate) fn new(
        logical_pos: (i32, i32),
        logical_size: (i32, i32),
        mode: (i32, i32),
        swaps_axes: bool,
    ) -> Self {
        Self {
            logical_pos,
            logical_size,
            mode,
            swaps_axes,
        }
    }

    fn contains(&self, x: i32, y: i32) -> bool {
        let (lx, ly) = self.logical_pos;
        let (lw, lh) = self.logical_size;
        x >= lx && x < lx.saturating_add(lw) && y >= ly && y < ly.saturating_add(lh)
    }

    /// physical/logical width as an exact rational, transform-corrected.
    fn scale(&self) -> Option<Scale120> {
        let physical_w = if self.swaps_axes {
            self.mode.1
        } else {
            self.mode.0
        };
        let physical_w = u32::try_from(physical_w).ok()?;
        let logical_w = NonZeroU32::new(u32::try_from(self.logical_size.0).ok()?)?;
        Scale120::from_physical_logical(physical_w, logical_w)
    }
}

/// Pure output selection + calculation. A point target falls back to the
/// first usable output when no output contains the point (the point may be a
/// stale position from a disconnected output).
pub(crate) fn select_scale(
    outputs: &[OutputCandidate],
    target: ProbeTarget,
) -> Result<Scale120, ScaleProbeError> {
    if let ProbeTarget::Point { x, y } = target
        && let Some(scale) = outputs
            .iter()
            .filter(|o| o.contains(x, y))
            .find_map(OutputCandidate::scale)
    {
        return Ok(scale);
    }
    outputs
        .iter()
        .find_map(OutputCandidate::scale)
        .ok_or(ScaleProbeError::NoUsableOutput)
}

struct State {
    registry_state: RegistryState,
    output_state: OutputState,
}

impl OutputHandler for State {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.output_state
    }
    fn new_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn update_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn output_destroyed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
}

impl ProvidesRegistryState for State {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry_state
    }
    registry_handlers![OutputState];
}

delegate_output!(State);
delegate_registry!(State);

fn transform_swaps_axes(t: wl_output::Transform) -> bool {
    matches!(
        t,
        wl_output::Transform::_90
            | wl_output::Transform::_270
            | wl_output::Transform::Flipped90
            | wl_output::Transform::Flipped270
    )
}

fn collect_candidates() -> Result<Vec<OutputCandidate>, ScaleProbeError> {
    if env::var_os("WAYLAND_DISPLAY").is_none() && env::var_os("WAYLAND_SOCKET").is_none() {
        return Err(ScaleProbeError::NoWaylandSession);
    }

    let conn = Connection::connect_to_env().map_err(|_| ScaleProbeError::Connection)?;
    let (globals, mut queue) =
        registry_queue_init::<State>(&conn).map_err(|_| ScaleProbeError::Connection)?;
    let qh = queue.handle();

    let mut state = State {
        registry_state: RegistryState::new(&globals),
        output_state: OutputState::new(&globals, &qh),
    };

    queue
        .roundtrip(&mut state)
        .map_err(|_| ScaleProbeError::Connection)?;
    queue
        .roundtrip(&mut state)
        .map_err(|_| ScaleProbeError::Connection)?;

    let mut candidates = Vec::new();
    for output in state.output_state.outputs() {
        // Incomplete metadata skips this output, not the whole probe.
        let Some(info) = state.output_state.info(&output) else {
            continue;
        };
        let (Some(pos), Some(size)) = (info.logical_position, info.logical_size) else {
            continue;
        };
        let Some(mode) = info
            .modes
            .iter()
            .find(|m| m.current)
            .or_else(|| info.modes.first())
        else {
            continue;
        };
        candidates.push(OutputCandidate::new(
            pos,
            size,
            mode.dimensions,
            transform_swaps_axes(info.transform),
        ));
    }
    Ok(candidates)
}

/// Query the live display and derive the scale for `target`.
pub(crate) fn probe_scale(target: ProbeTarget) -> Result<Scale120, ScaleProbeError> {
    select_scale(&collect_candidates()?, target)
}

/// [`probe_scale`] on a throwaway thread, waiting at most `timeout`. The probe
/// round-trips on a second display connection, which can block indefinitely if
/// the compositor stops responding; the caller must never do that inline (it
/// would stall the root event loop), so the blocking part is isolated here and
/// abandoned on timeout — the orphaned thread holds only its own private
/// connection and exits with the process.
pub(crate) fn probe_scale_bounded(
    target: ProbeTarget,
    timeout: std::time::Duration,
) -> Result<Scale120, ScaleProbeError> {
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::Builder::new()
        .name("wl-scale-probe".into())
        .spawn(move || {
            let _ = tx.send(probe_scale(target));
        })
        .map_err(|_| ScaleProbeError::Connection)?;
    rx.recv_timeout(timeout)
        .map_err(|_| ScaleProbeError::Timeout)?
}

#[cfg(test)]
mod tests {
    use super::*;

    fn landscape() -> OutputCandidate {
        // 3840x2160 panel at 1.5: logical 2560x1440 at (0,0).
        OutputCandidate::new((0, 0), (2560, 1440), (3840, 2160), false)
    }

    fn portrait() -> OutputCandidate {
        // Same panel rotated 90°: logical 1440x2560 at (2560,0); mode stays
        // in native axes, so the logical width maps to the mode HEIGHT.
        OutputCandidate::new((2560, 0), (1440, 2560), (3840, 2160), true)
    }

    fn scale_of(r: Result<Scale120, ScaleProbeError>) -> f32 {
        r.unwrap().ratio_f32()
    }

    #[test]
    fn first_output_uses_first_usable() {
        assert_eq!(
            scale_of(select_scale(
                &[landscape(), portrait()],
                ProbeTarget::FirstOutput
            )),
            1.5
        );
    }

    #[test]
    fn point_selects_containing_output() {
        let outs = [landscape(), portrait()];
        assert_eq!(
            scale_of(select_scale(&outs, ProbeTarget::Point { x: 100, y: 100 })),
            1.5
        );
        assert_eq!(
            scale_of(select_scale(&outs, ProbeTarget::Point { x: 2560, y: 0 })),
            1.5
        );
    }

    #[test]
    fn rotated_output_uses_swapped_mode_axis() {
        // Without transform awareness this would compute 3840/1440 ≈ 2.67
        // instead of 2160/1440 = 1.5.
        assert_eq!(
            scale_of(select_scale(&[portrait()], ProbeTarget::FirstOutput)),
            1.5
        );
    }

    #[test]
    fn point_outside_every_output_falls_back_to_first() {
        assert_eq!(
            scale_of(select_scale(
                &[landscape()],
                ProbeTarget::Point { x: -5000, y: -5000 }
            )),
            1.5
        );
    }

    #[test]
    fn no_outputs_is_an_error() {
        assert_eq!(
            select_scale(&[], ProbeTarget::FirstOutput),
            Err(ScaleProbeError::NoUsableOutput)
        );
    }

    #[test]
    fn degenerate_geometry_is_skipped_not_divided_by() {
        let zero_logical = OutputCandidate::new((0, 0), (0, 0), (3840, 2160), false);
        let negative_mode = OutputCandidate::new((0, 0), (2560, 1440), (-1, -1), false);
        assert_eq!(
            select_scale(&[zero_logical, negative_mode], ProbeTarget::FirstOutput),
            Err(ScaleProbeError::NoUsableOutput)
        );
        // A later healthy output still wins.
        assert_eq!(
            scale_of(select_scale(
                &[zero_logical, landscape()],
                ProbeTarget::FirstOutput
            )),
            1.5
        );
    }

    #[test]
    fn unusable_containing_output_falls_back() {
        // The point hits an output with broken geometry; selection falls back
        // to the first usable output instead of failing.
        let broken_at_origin = OutputCandidate::new((0, 0), (2560, 1440), (0, 0), false);
        assert_eq!(
            scale_of(select_scale(
                &[broken_at_origin, portrait()],
                ProbeTarget::Point { x: 10, y: 10 }
            )),
            1.5
        );
    }

    #[test]
    fn huge_extents_do_not_overflow_containment() {
        let o = OutputCandidate::new((i32::MAX - 10, 0), (i32::MAX, 100), (100, 100), false);
        assert!(o.contains(i32::MAX - 1, 50));
    }
}
