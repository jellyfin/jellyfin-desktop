//! Theme-color sink. Resets ThemeColor video mode on terminal playback
//! events. Active-true setVideoMode fires from the C++ web_browser path
//! on metadata arrival; that's not mpv-derived and stays out of the
//! playback event stream.
//!
//! Replaces `src/playback/sinks/theme_color_sink.cpp`.

use std::sync::{Mutex, OnceLock};

use crate::types::{PlaybackEvent, PlaybackEventKind};

type SetCb = extern "C" fn(bool);

fn cb_slot() -> &'static Mutex<Option<SetCb>> {
    static SLOT: OnceLock<Mutex<Option<SetCb>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

/// Install the ThemeColor::setVideoMode setter. `cb == None` disables.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_theme_video_mode_handler(cb: Option<SetCb>) {
    *cb_slot().lock().unwrap() = cb;
}

pub(crate) fn deliver(ev: &PlaybackEvent) {
    match ev.kind {
        PlaybackEventKind::Finished | PlaybackEventKind::Canceled | PlaybackEventKind::Error => {
            if let Some(cb) = *cb_slot().lock().unwrap() {
                cb(false);
            }
        }
        _ => {}
    }
}
