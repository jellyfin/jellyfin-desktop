//! Browser playback sink. Forwards UI-affecting events to the embedded
//! web view via the exec_js callback the C++ side installs. Reads only
//! from the event snapshot.
//!
//! Replaces `src/playback/sinks/browser_sink.cpp`.

use std::sync::{Mutex, OnceLock};

use serde_json::json;

use crate::exec_js::call as call_exec_js;
use crate::types::{PlaybackEvent, PlaybackEventKind};

type SetSizeCb = extern "C" fn(i32, i32, i32, i32);
type SetHzCb = extern "C" fn(f64);
type SetMaxCb = extern "C" fn(bool);

struct Handlers {
    set_size: Option<SetSizeCb>,
    set_hz: Option<SetHzCb>,
    set_max: Option<SetMaxCb>,
}

fn slot() -> &'static Mutex<Handlers> {
    static SLOT: OnceLock<Mutex<Handlers>> = OnceLock::new();
    SLOT.get_or_init(|| {
        Mutex::new(Handlers {
            set_size: None,
            set_hz: None,
            set_max: None,
        })
    })
}

/// Install / clear the browsers.setSize handler.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_browsers_size_handler(cb: Option<SetSizeCb>) {
    slot().lock().unwrap().set_size = cb;
}

/// Install / clear the browsers.setRefreshRate handler.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_browsers_refresh_rate_handler(cb: Option<SetHzCb>) {
    slot().lock().unwrap().set_hz = cb;
}

/// Install / clear the was-maximized-before-fullscreen writer.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_was_maximized_handler(cb: Option<SetMaxCb>) {
    slot().lock().unwrap().set_max = cb;
}

pub(crate) fn deliver(ev: &PlaybackEvent) {
    let snap = &ev.snapshot;
    match ev.kind {
        PlaybackEventKind::Started => call_exec_js("window._nativeEmit('playing')"),
        PlaybackEventKind::Paused => call_exec_js("window._nativeEmit('paused')"),
        PlaybackEventKind::Finished => call_exec_js("window._nativeEmit('finished')"),
        PlaybackEventKind::Canceled => call_exec_js("window._nativeEmit('canceled')"),
        PlaybackEventKind::Error => {
            let msg = if ev.error_message.is_empty() {
                "Playback error"
            } else {
                ev.error_message.as_str()
            };
            let json_str = json!(msg).to_string();
            call_exec_js(&format!("window._nativeEmit('error',{})", json_str));
        }
        PlaybackEventKind::SeekingChanged => {
            if ev.flag {
                call_exec_js("window._nativeEmit('seeking')");
            }
        }
        PlaybackEventKind::TrackLoaded => {
            // Variant switch (same Jellyfin Id): JS's playerLoad path doesn't
            // fire its own pause UI, so drive the pause indicator from here.
            // Cleared on first-frame Started via the Started → 'playing' emit.
            if snap.variant_switch_pending {
                call_exec_js("window._nativeEmit('paused')");
            }
        }
        PlaybackEventKind::PositionChanged => {
            let ms = (snap.position_us / 1000) as i32;
            call_exec_js(&format!("window._nativeUpdatePosition({})", ms));
        }
        PlaybackEventKind::DurationChanged => {
            let ms = (snap.duration_us / 1000) as i32;
            call_exec_js(&format!("window._nativeUpdateDuration({})", ms));
        }
        PlaybackEventKind::RateChanged => {
            call_exec_js(&format!("window._nativeSetRate({})", snap.rate));
        }
        PlaybackEventKind::FullscreenChanged => {
            // Mirror was-maximized so the geometry-save tail in main.cpp can
            // read it after coord shutdown without keeping coord alive.
            if let Some(cb) = slot().lock().unwrap().set_max {
                cb(snap.maximized_before_fullscreen);
            }
            call_exec_js(&format!(
                "window._nativeFullscreenChanged({})",
                if snap.fullscreen { "true" } else { "false" }
            ));
        }
        PlaybackEventKind::OsdDimsChanged => {
            if let Some(cb) = slot().lock().unwrap().set_size {
                cb(snap.layout_w, snap.layout_h, snap.pixel_w, snap.pixel_h);
            }
        }
        PlaybackEventKind::DisplayHzChanged => {
            if let Some(cb) = slot().lock().unwrap().set_hz {
                cb(snap.display_hz);
            }
        }
        PlaybackEventKind::BufferedRangesChanged => {
            let arr: Vec<_> = snap
                .buffered
                .iter()
                .map(|r| json!({ "start": r.start_ticks, "end": r.end_ticks }))
                .collect();
            let json_str = serde_json::Value::Array(arr).to_string();
            call_exec_js(&format!(
                "window._nativeUpdateBufferedRanges({})",
                json_str
            ));
        }
        PlaybackEventKind::BufferingChanged
        | PlaybackEventKind::MediaTypeChanged
        | PlaybackEventKind::MetadataChanged
        | PlaybackEventKind::ArtworkChanged
        | PlaybackEventKind::QueueCapsChanged
        | PlaybackEventKind::Seeked => {
            // Not surfaced via this sink. JS already owns metadata.
        }
    }
}

