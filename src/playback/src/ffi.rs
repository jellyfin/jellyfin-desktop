//! Public Rust API for the playback module.
//!
//! Sinks register typed closures (`Box<dyn Fn(&PlaybackEvent) + Send +
//! Sync>`); the coordinator worker fans events out by invoking them
//! directly. Producers call [`post`] with a typed [`Input`].

use parking_lot::Mutex;
use std::sync::OnceLock;

pub use crate::coordinator::Input;
use crate::coordinator::{CoordinatorHandle, PlaybackCoordinator};
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
    let guard = coord_slot().lock();
    if let Some(c) = guard.as_ref() {
        f(c);
    }
}

fn coord_handle() -> Option<CoordinatorHandle> {
    coord_slot()
        .lock()
        .as_ref()
        .map(PlaybackCoordinator::handle)
}

pub fn jfn_playback_init() {
    {
        let mut guard = coord_slot().lock();
        if guard.is_some() {
            return;
        }
        let Some(mut c) = PlaybackCoordinator::new() else {
            eprintln!("[playback] failed to create coordinator (wake eventfd)");
            return;
        };
        register_builtin_sinks(&c);
        c.start();
        *guard = Some(c);
    }
    // The immediate reconcile is load-bearing: mode posts made before the
    // coordinator existed were dropped by `post`, and no wakeup replays them.
    jfn_platform_abi::subscribe_window_changed(
        crate::ingest_driver::jfn_playback_reconcile_window_mode,
    );
    crate::ingest_driver::jfn_playback_reconcile_window_mode();
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
}

pub fn jfn_playback_shutdown() {
    // stop() joins the worker, whose sinks call post() → coord_slot;
    // holding the guard across stop() deadlocks.
    let coord = coord_slot().lock().take();
    if let Some(mut c) = coord {
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
    coord_handle().map_or_else(PlaybackSnapshot::fresh, |h| h.snapshot())
}

// =====================================================================
// Producer entry point
// =====================================================================

pub fn post(input: Input) {
    if let Some(h) = coord_handle() {
        h.enqueue(input);
    }
}
