//! OS media-session integration (MPRIS / MPNowPlaying / SMTC).
//!
//! Sinks are consumers of playback state only — mpv remains the
//! authoritative source; a sink never determines playback state.

pub trait MediaSink: Send + Sync {
    /// Must run after the playback coordinator is initialized: sinks
    /// register their event delivery with the coordinator here, and
    /// registration on a missing coordinator is silently dropped.
    fn start(&self);
    fn stop(&self);
}
