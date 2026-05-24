//! Idle-inhibit sink. Watches phase + media_type transitions and drives
//! the platform idle-inhibit level via a reverse-FFI callback the C++
//! side installs at startup (wired to `g_platform.set_idle_inhibit`).
//!
//! Replaces `src/playback/sinks/idle_inhibit_sink.cpp`.

use std::sync::{Mutex, OnceLock};

use crate::types::{MediaType, PlaybackEvent, PlaybackEventKind, PlaybackPhase, PlaybackSnapshot};

// Mirrors C++ `enum class IdleInhibitLevel { None, System, Display }`.
const LEVEL_NONE: u32 = 0;
const LEVEL_SYSTEM: u32 = 1;
const LEVEL_DISPLAY: u32 = 2;

type SetCb = extern "C" fn(u32);

fn cb_slot() -> &'static Mutex<Option<SetCb>> {
    static SLOT: OnceLock<Mutex<Option<SetCb>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

/// Install the platform idle-inhibit setter. `cb == None` disables the sink.
pub fn jfn_playback_set_idle_inhibit_handler(cb: Option<SetCb>) {
    *cb_slot().lock().unwrap() = cb;
}

fn apply(snap: &PlaybackSnapshot) {
    let Some(cb) = *cb_slot().lock().unwrap() else {
        return;
    };
    let level = if snap.phase != PlaybackPhase::Playing {
        LEVEL_NONE
    } else if snap.media_type == MediaType::Audio {
        LEVEL_SYSTEM
    } else {
        LEVEL_DISPLAY
    };
    cb(level);
}

pub(crate) fn deliver(ev: &PlaybackEvent) {
    match ev.kind {
        PlaybackEventKind::Started
        | PlaybackEventKind::Paused
        | PlaybackEventKind::Finished
        | PlaybackEventKind::Canceled
        | PlaybackEventKind::Error
        | PlaybackEventKind::MediaTypeChanged => apply(&ev.snapshot),
        _ => {}
    }
}
