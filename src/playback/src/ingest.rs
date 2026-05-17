//! Pure-Rust port of `src/mpv/event.cpp:digest_property` (and the
//! end-file / file-loaded / shutdown decoding in `mpv_digest_thread`).
//!
//! Consumes [`mpv::Event`] values from the Rust event loop and produces
//! coordinator [`Input`]s plus a couple of side outputs that don't fit
//! the [`Input`] vocabulary (display-scale callback fanout, raw OSD
//! pixel-dim mirror for the geometry-save cache).
//!
//! State that mirrored the C++ `s_*` atomics (fullscreen, window_max,
//! display_scale, display_hz) lives in [`IngestState`] so multiple
//! ingest calls observe the same change-suppression behavior.

use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};

use jfn_mpv::{Event, ObserveId, PropertyValue};

use crate::coordinator::Input;
use crate::types::{EndReason, PlaybackBufferedRange};

/// Property observe-IDs passed to `mpv_observe_property`. Mirrors
/// `enum MpvObserveId` in `src/mpv/event.h` so the C++ side and the Rust
/// digest agree on which ID maps to which property.
pub mod observe_id {
    pub const OSD_DIMS: u64 = 2;
    pub const FULLSCREEN: u64 = 3;
    pub const PAUSE: u64 = 4;
    pub const TIME_POS: u64 = 5;
    pub const DURATION: u64 = 6;
    pub const SPEED: u64 = 7;
    pub const SEEKING: u64 = 8;
    pub const DISPLAY_FPS: u64 = 9;
    pub const CACHE_STATE: u64 = 10;
    pub const WINDOW_MAX: u64 = 11;
    pub const DISPLAY_SCALE: u64 = 12;
    pub const PAUSED_FOR_CACHE: u64 = 13;
    pub const CORE_IDLE: u64 = 14;
    pub const VIDEO_FRAME_INFO: u64 = 15;
}

const MAX_BUFFERED_RANGES: usize = 8;

/// Caller-provided platform hooks. Matches the surface the C++ digest
/// reached out to (`g_platform.get_scale`, `macos_platform::
/// query_logical_content_size`). Implementations stay outside this
/// crate so jfn-playback doesn't grow a platform dep.
pub trait IngestCtx {
    /// Current device pixel scale. `0.0` is treated as unknown and
    /// substituted with `1.0` by [`ingest`].
    fn scale(&self) -> f32;

    /// macOS-only logical content size override. When `Some`, the OSD
    /// dim emitted to the coordinator uses this for `lw`/`lh` and back-
    /// computes `pw`/`ph` from `scale`. Returns `None` on every other
    /// platform.
    fn macos_logical_size(&self) -> Option<(i32, i32)> {
        None
    }
}

/// One ingest-loop output. Most map to coordinator inputs; the two side
/// variants exist because the prior C++ path didn't route them through
/// the dispatcher queue either.
#[derive(Debug)]
pub(crate) enum IngestOut {
    Input(Input),
    /// `mpv::display-hidpi-scale` changed. Forwarded to the
    /// browser-side `setScale` handler.
    DisplayScaleChanged(f64),
    /// Terminal: libmpv has shut down. Caller breaks out of the event
    /// loop and triggers the rest of the app's teardown.
    Shutdown,
}

/// Shared atomic cache mirroring the prior C++ `s_*` statics. Holds
/// last-observed values so digest functions can suppress duplicate
/// emissions (display-scale, display-fps) and so external readers
/// (`fullscreen`, `window_maximized`, `display_scale`, `display_hz`)
/// see the current state without round-tripping through the
/// coordinator snapshot.
#[derive(Debug, Default)]
pub struct IngestState {
    fullscreen: AtomicBool,
    window_maximized: AtomicBool,
    osd_pw: AtomicI64,
    osd_ph: AtomicI64,
    /// Last device-pixel window size the app pushed (boot geometry,
    /// runtime resize, or the most recent osd-dimensions event). Read
    /// by the geometry-save tail at shutdown.
    window_pw: AtomicI64,
    window_ph: AtomicI64,
    /// `f64` bit pattern stored as `u64` — `AtomicF64` isn't stable.
    display_scale_bits: AtomicU64,
    display_hz_bits: AtomicU64,
}

impl IngestState {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn fullscreen(&self) -> bool {
        self.fullscreen.load(Ordering::Relaxed)
    }
    pub fn window_maximized(&self) -> bool {
        self.window_maximized.load(Ordering::Relaxed)
    }
    pub fn osd_pw(&self) -> i32 {
        self.osd_pw.load(Ordering::Relaxed) as i32
    }
    pub fn osd_ph(&self) -> i32 {
        self.osd_ph.load(Ordering::Relaxed) as i32
    }
    pub fn window_pw(&self) -> i32 {
        self.window_pw.load(Ordering::Relaxed) as i32
    }
    pub fn window_ph(&self) -> i32 {
        self.window_ph.load(Ordering::Relaxed) as i32
    }
    pub fn set_window_pixels(&self, pw: i32, ph: i32) {
        self.window_pw.store(pw as i64, Ordering::Relaxed);
        self.window_ph.store(ph as i64, Ordering::Relaxed);
    }
    pub fn display_scale(&self) -> f64 {
        f64::from_bits(self.display_scale_bits.load(Ordering::Relaxed))
    }
    pub fn display_hz(&self) -> f64 {
        f64::from_bits(self.display_hz_bits.load(Ordering::Relaxed))
    }
    pub fn set_display_hz(&self, hz: f64) {
        self.display_hz_bits.store(hz.to_bits(), Ordering::Relaxed);
    }
}

/// Decode one [`Event`] into zero or more [`IngestOut`]s. Mirrors
/// `mpv_digest_thread` in `src/main.cpp` plus `digest_property` in
/// `src/mpv/event.cpp`.
/// Re-exported under stable FFI-facing name for [`crate::ingest_driver`].
pub(crate) fn ingest_event_for_ffi<C: IngestCtx>(
    event: &Event,
    state: &IngestState,
    ctx: &C,
) -> Vec<IngestOut> {
    ingest(event, state, ctx)
}

/// Run only the property-digest path. Used by the Wayland fast path
/// that synthesizes osd-dimension updates outside the mpv event stream.
pub(crate) fn ingest_property_for_ffi<C: IngestCtx>(
    id: ObserveId,
    value: &PropertyValue,
    state: &IngestState,
    ctx: &C,
) -> Vec<IngestOut> {
    digest_property(id, value, state, ctx)
}

pub(crate) fn ingest<C: IngestCtx>(
    event: &Event,
    state: &IngestState,
    ctx: &C,
) -> Vec<IngestOut> {
    match event {
        Event::Shutdown => vec![IngestOut::Shutdown],
        Event::FileLoaded => vec![IngestOut::Input(Input::FileLoaded)],
        Event::EndFile(reason) => Some(end_file_input(reason))
            .into_iter()
            .map(IngestOut::Input)
            .collect(),
        Event::PropertyChange { id, value, .. } => digest_property(*id, value, state, ctx),
        _ => Vec::new(),
    }
}

fn end_file_input(reason: &jfn_mpv::EndFileReason) -> Input {
    use jfn_mpv::EndFileReason as R;
    match reason {
        R::Eof | R::Redirect => Input::EndFile {
            reason: EndReason::Eof,
            error_message: String::new(),
        },
        R::Stop | R::Quit => Input::EndFile {
            reason: EndReason::Canceled,
            error_message: String::new(),
        },
        R::Error(e) => Input::EndFile {
            reason: EndReason::Error,
            error_message: e.to_string(),
        },
        R::Unknown(_) => Input::EndFile {
            reason: EndReason::Canceled,
            error_message: String::new(),
        },
    }
}

fn digest_property<C: IngestCtx>(
    id: ObserveId,
    value: &PropertyValue,
    state: &IngestState,
    ctx: &C,
) -> Vec<IngestOut> {
    use observe_id::*;
    match id {
        OSD_DIMS => digest_osd_dims(value, state, ctx)
            .into_iter()
            .collect(),
        PAUSE => as_flag(value)
            .map(|f| vec![IngestOut::Input(Input::PauseChanged(f))])
            .unwrap_or_default(),
        TIME_POS => as_double(value)
            .map(|d| {
                vec![IngestOut::Input(Input::Position(
                    (d * 1_000_000.0) as i64,
                ))]
            })
            .unwrap_or_default(),
        DURATION => as_double(value)
            .map(|d| {
                vec![IngestOut::Input(Input::Duration(
                    (d * 1_000_000.0) as i64,
                ))]
            })
            .unwrap_or_default(),
        FULLSCREEN => match as_flag(value) {
            Some(f) => {
                state.fullscreen.store(f, Ordering::Relaxed);
                vec![IngestOut::Input(Input::Fullscreen {
                    fullscreen: f,
                    was_maximized: if f { state.window_maximized() } else { false },
                })]
            }
            None => Vec::new(),
        },
        SPEED => as_double(value)
            .map(|d| vec![IngestOut::Input(Input::Speed(d))])
            .unwrap_or_default(),
        SEEKING => as_flag(value)
            .map(|f| vec![IngestOut::Input(Input::SeekingChanged(f))])
            .unwrap_or_default(),
        PAUSED_FOR_CACHE => as_flag(value)
            .map(|f| vec![IngestOut::Input(Input::PausedForCache(f))])
            .unwrap_or_default(),
        CORE_IDLE => as_flag(value)
            .map(|f| vec![IngestOut::Input(Input::CoreIdle(f))])
            .unwrap_or_default(),
        VIDEO_FRAME_INFO => vec![IngestOut::Input(Input::VideoFrameAvailable(
            !matches!(value, PropertyValue::None),
        ))],
        WINDOW_MAX => {
            if let Some(f) = as_flag(value) {
                state.window_maximized.store(f, Ordering::Relaxed);
            }
            Vec::new()
        }
        DISPLAY_SCALE => {
            let Some(new_scale) = as_double(value) else {
                return Vec::new();
            };
            let old_bits = state
                .display_scale_bits
                .swap(new_scale.to_bits(), Ordering::Relaxed);
            if f64::from_bits(old_bits) != new_scale {
                vec![IngestOut::DisplayScaleChanged(new_scale)]
            } else {
                Vec::new()
            }
        }
        DISPLAY_FPS => {
            let Some(fps) = as_double(value) else {
                return Vec::new();
            };
            if fps != state.display_hz() {
                state
                    .display_hz_bits
                    .store(fps.to_bits(), Ordering::Relaxed);
                vec![IngestOut::Input(Input::DisplayHz(fps))]
            } else {
                Vec::new()
            }
        }
        CACHE_STATE => digest_cache_state(value),
        _ => Vec::new(),
    }
}

fn digest_osd_dims<C: IngestCtx>(
    value: &PropertyValue,
    state: &IngestState,
    ctx: &C,
) -> Vec<IngestOut> {
    let PropertyValue::Node(node) = value else {
        return Vec::new();
    };
    let w = node.get("w").and_then(|v| v.as_int()).unwrap_or(0);
    let h = node.get("h").and_then(|v| v.as_int()).unwrap_or(0);
    if w <= 0 || h <= 0 {
        return Vec::new();
    }
    let mut pw = w as i32;
    let mut ph = h as i32;
    state.osd_pw.store(pw as i64, Ordering::Relaxed);
    state.osd_ph.store(ph as i64, Ordering::Relaxed);
    let scale = {
        let s = ctx.scale();
        if s > 0.0 { s } else { 1.0 }
    };
    let mut lw = (pw as f32 / scale) as i32;
    let mut lh = (ph as f32 / scale) as i32;
    if let Some((qlw, qlh)) = ctx.macos_logical_size() {
        if qlw > 0 && qlh > 0 {
            lw = qlw;
            lh = qlh;
            pw = (qlw as f32 * scale) as i32;
            ph = (qlh as f32 * scale) as i32;
        }
    }
    if lw <= 0 || lh <= 0 {
        return Vec::new();
    }
    // Keep the effective-pixel cache current so a later geometry save
    // reads the latest resize rather than the boot-time seed.
    state.set_window_pixels(pw, ph);
    vec![IngestOut::Input(Input::OsdDims { lw, lh, pw, ph })]
}

fn digest_cache_state(value: &PropertyValue) -> Vec<IngestOut> {
    let PropertyValue::Node(node) = value else {
        return Vec::new();
    };
    let Some(arr) = node.get("seekable-ranges").and_then(|v| v.as_array()) else {
        return Vec::new();
    };
    let mut ranges = Vec::with_capacity(arr.len().min(MAX_BUFFERED_RANGES));
    for range in arr.iter().take(MAX_BUFFERED_RANGES) {
        let start = range.get("start").and_then(|v| v.as_double()).unwrap_or(0.0);
        let end = range.get("end").and_then(|v| v.as_double()).unwrap_or(0.0);
        ranges.push(PlaybackBufferedRange {
            start_ticks: (start * 10_000_000.0) as i64,
            end_ticks: (end * 10_000_000.0) as i64,
        });
    }
    vec![IngestOut::Input(Input::BufferedRanges(ranges))]
}

fn as_flag(v: &PropertyValue) -> Option<bool> {
    if let PropertyValue::Flag(f) = v {
        Some(*f)
    } else {
        None
    }
}

fn as_double(v: &PropertyValue) -> Option<f64> {
    if let PropertyValue::Double(d) = v {
        Some(*d)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use jfn_mpv::Node;

    struct TestCtx {
        scale: f32,
        mac: Option<(i32, i32)>,
    }
    impl IngestCtx for TestCtx {
        fn scale(&self) -> f32 {
            self.scale
        }
        fn macos_logical_size(&self) -> Option<(i32, i32)> {
            self.mac
        }
    }

    fn ctx(scale: f32) -> TestCtx {
        TestCtx { scale, mac: None }
    }

    fn prop(id: u64, value: PropertyValue) -> Event {
        Event::PropertyChange {
            id,
            name: String::new(),
            value,
        }
    }

    #[test]
    fn pause_flag_round_trips() {
        let state = IngestState::new();
        let out = ingest(
            &prop(observe_id::PAUSE, PropertyValue::Flag(true)),
            &state,
            &ctx(1.0),
        );
        assert_eq!(out.len(), 1);
        matches!(out[0], IngestOut::Input(Input::PauseChanged(true)));
    }

    #[test]
    fn time_pos_scales_to_microseconds() {
        let state = IngestState::new();
        let out = ingest(
            &prop(observe_id::TIME_POS, PropertyValue::Double(1.5)),
            &state,
            &ctx(1.0),
        );
        let IngestOut::Input(Input::Position(p)) = &out[0] else {
            panic!("expected Position");
        };
        assert_eq!(*p, 1_500_000);
    }

    #[test]
    fn fullscreen_carries_maximized_when_entering() {
        let state = IngestState::new();
        // Window first reports maximized true.
        let _ = ingest(
            &prop(observe_id::WINDOW_MAX, PropertyValue::Flag(true)),
            &state,
            &ctx(1.0),
        );
        let out = ingest(
            &prop(observe_id::FULLSCREEN, PropertyValue::Flag(true)),
            &state,
            &ctx(1.0),
        );
        let IngestOut::Input(Input::Fullscreen {
            fullscreen,
            was_maximized,
        }) = out[0]
        else {
            panic!("expected Fullscreen");
        };
        assert!(fullscreen);
        assert!(was_maximized);

        // Leaving fullscreen always reports was_maximized = false.
        let out = ingest(
            &prop(observe_id::FULLSCREEN, PropertyValue::Flag(false)),
            &state,
            &ctx(1.0),
        );
        let IngestOut::Input(Input::Fullscreen {
            fullscreen,
            was_maximized,
        }) = out[0]
        else {
            panic!("expected Fullscreen");
        };
        assert!(!fullscreen);
        assert!(!was_maximized);
        assert!(!state.fullscreen());
    }

    #[test]
    fn display_scale_suppresses_duplicates() {
        let state = IngestState::new();
        let v = PropertyValue::Double(2.0);
        let out = ingest(&prop(observe_id::DISPLAY_SCALE, v.clone()), &state, &ctx(1.0));
        assert!(matches!(out[0], IngestOut::DisplayScaleChanged(s) if s == 2.0));
        let out = ingest(&prop(observe_id::DISPLAY_SCALE, v), &state, &ctx(1.0));
        assert!(out.is_empty());
    }

    #[test]
    fn display_fps_suppresses_duplicates() {
        let state = IngestState::new();
        let v = PropertyValue::Double(60.0);
        let out = ingest(&prop(observe_id::DISPLAY_FPS, v.clone()), &state, &ctx(1.0));
        matches!(out[0], IngestOut::Input(Input::DisplayHz(_)));
        assert_eq!(state.display_hz(), 60.0);
        let out = ingest(&prop(observe_id::DISPLAY_FPS, v), &state, &ctx(1.0));
        assert!(out.is_empty());
    }

    #[test]
    fn osd_dims_emits_logical_and_pixel_pairs() {
        let state = IngestState::new();
        let node = Node::Map(vec![
            ("w".into(), Node::Int(3840)),
            ("h".into(), Node::Int(2160)),
        ]);
        let out = ingest(
            &prop(observe_id::OSD_DIMS, PropertyValue::Node(node)),
            &state,
            &ctx(2.0),
        );
        assert_eq!(out.len(), 1);
        let IngestOut::Input(Input::OsdDims { lw, lh, pw, ph }) = out[0] else {
            panic!("expected OsdDims");
        };
        assert_eq!((lw, lh, pw, ph), (1920, 1080, 3840, 2160));
        assert_eq!(state.osd_pw(), 3840);
        assert_eq!(state.osd_ph(), 2160);
        assert_eq!(state.window_pw(), 3840);
        assert_eq!(state.window_ph(), 2160);
    }

    #[test]
    fn osd_dims_uses_macos_logical_override() {
        let state = IngestState::new();
        let mut c = ctx(2.0);
        c.mac = Some((1280, 720));
        let node = Node::Map(vec![
            ("w".into(), Node::Int(3840)),
            ("h".into(), Node::Int(2160)),
        ]);
        let out = ingest(
            &prop(observe_id::OSD_DIMS, PropertyValue::Node(node)),
            &state,
            &c,
        );
        let IngestOut::Input(Input::OsdDims { lw, lh, pw, ph }) = out[0] else {
            panic!();
        };
        assert_eq!((lw, lh, pw, ph), (1280, 720, 2560, 1440));
    }

    #[test]
    fn osd_dims_rejects_non_positive() {
        let state = IngestState::new();
        let node = Node::Map(vec![
            ("w".into(), Node::Int(0)),
            ("h".into(), Node::Int(1080)),
        ]);
        let out = ingest(
            &prop(observe_id::OSD_DIMS, PropertyValue::Node(node)),
            &state,
            &ctx(1.0),
        );
        assert!(out.is_empty());
    }

    #[test]
    fn cache_state_extracts_seekable_ranges() {
        let state = IngestState::new();
        let range = Node::Map(vec![
            ("start".into(), Node::Double(0.0)),
            ("end".into(), Node::Double(2.5)),
        ]);
        let root = Node::Map(vec![(
            "seekable-ranges".into(),
            Node::Array(vec![range.clone(), range]),
        )]);
        let out = ingest(
            &prop(observe_id::CACHE_STATE, PropertyValue::Node(root)),
            &state,
            &ctx(1.0),
        );
        let IngestOut::Input(Input::BufferedRanges(ref r)) = out[0] else {
            panic!("expected BufferedRanges");
        };
        assert_eq!(r.len(), 2);
        assert_eq!(r[0].start_ticks, 0);
        assert_eq!(r[0].end_ticks, 25_000_000);
    }

    #[test]
    fn shutdown_event_returns_terminal() {
        let state = IngestState::new();
        let out = ingest(&Event::Shutdown, &state, &ctx(1.0));
        assert!(matches!(out[0], IngestOut::Shutdown));
    }

    #[test]
    fn end_file_maps_reason() {
        let state = IngestState::new();
        let out = ingest(
            &Event::EndFile(jfn_mpv::EndFileReason::Eof),
            &state,
            &ctx(1.0),
        );
        let IngestOut::Input(Input::EndFile { reason, .. }) = &out[0] else {
            panic!();
        };
        assert_eq!(*reason, EndReason::Eof);
    }

    #[test]
    fn file_loaded_emits_input() {
        let state = IngestState::new();
        let out = ingest(&Event::FileLoaded, &state, &ctx(1.0));
        assert!(matches!(out[0], IngestOut::Input(Input::FileLoaded)));
    }
}
