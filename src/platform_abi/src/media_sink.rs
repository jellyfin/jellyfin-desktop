//! OS media-session integration (MPRIS / MPNowPlaying / SMTC).
//!
//! Sinks are consumers of playback state only — mpv remains the
//! authoritative source; a sink never determines playback state.

pub trait MediaSink: Send + Sync {
    fn start(&self);
    fn stop(&self);
}
