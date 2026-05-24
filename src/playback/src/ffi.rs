//! Public Rust API for the playback module.
//!
//! Sinks register typed closures (`Box<dyn Fn(&PlaybackEvent) + Send +
//! Sync>`); the coordinator worker fans events out by invoking them
//! directly. Producers call [`post`] with a typed [`Input`].

use std::sync::{Mutex, OnceLock};

pub use crate::coordinator::Input;
use crate::coordinator::PlaybackCoordinator;
use crate::types::*;

// =====================================================================
// Sink closure types
// =====================================================================

pub type EventSink = Box<dyn Fn(&PlaybackEvent) + Send + Sync>;
pub type ActionSink = Box<dyn Fn(&PlaybackAction) + Send + Sync>;

// =====================================================================
// Singleton coordinator
// =====================================================================

static COORD: OnceLock<Mutex<Option<PlaybackCoordinator>>> = OnceLock::new();

pub(crate) fn coord_slot() -> &'static Mutex<Option<PlaybackCoordinator>> {
    COORD.get_or_init(|| Mutex::new(None))
}

fn with_coord<F: FnOnce(&PlaybackCoordinator)>(f: F) {
    let guard = coord_slot().lock().unwrap();
    if let Some(c) = guard.as_ref() {
        f(c);
    }
}

pub fn jfn_playback_init() {
    let mut guard = coord_slot().lock().unwrap();
    if guard.is_none() {
        let mut c = PlaybackCoordinator::new();
        register_builtin_sinks(&c);
        c.start();
        *guard = Some(c);
    }
}

fn register_builtin_sinks(c: &PlaybackCoordinator) {
    c.add_builtin_action_sink(Box::new(|a: &PlaybackAction| match a.kind {
        PlaybackActionKind::ApplyPendingTrackSelectionAndPlay => {
            jfn_mpv::api::jfn_mpv_apply_pending_track_selection_and_play();
        }
    }));

    c.add_builtin_event_sink(Box::new(|ev: &PlaybackEvent| {
        crate::idle_inhibit_sink::deliver(ev);
    }));

    c.add_builtin_event_sink(Box::new(|ev: &PlaybackEvent| {
        crate::browser_sink::deliver(ev);
    }));

    c.add_builtin_event_sink(Box::new(|ev: &PlaybackEvent| {
        crate::theme_color_sink::deliver(ev);
    }));

    #[cfg(target_os = "linux")]
    c.add_builtin_event_sink(Box::new(|ev: &PlaybackEvent| {
        crate::mpris_sink::deliver(ev.clone());
    }));
}

pub fn jfn_playback_shutdown() {
    let mut guard = coord_slot().lock().unwrap();
    if let Some(mut c) = guard.take() {
        c.stop();
    }
}

pub fn register_event_sink(sink: EventSink) {
    with_coord(|c| c.add_event_sink(sink));
}

pub fn register_action_sink(sink: ActionSink) {
    with_coord(|c| c.add_action_sink(sink));
}

pub fn jfn_playback_snapshot() -> PlaybackSnapshot {
    let mut guard = coord_slot().lock().unwrap();
    match guard.as_mut() {
        Some(c) => c.snapshot(),
        None => PlaybackSnapshot::fresh(),
    }
}

// =====================================================================
// Producer entry point
// =====================================================================

pub fn post(input: Input) {
    with_coord(|c| c.enqueue(input));
}
