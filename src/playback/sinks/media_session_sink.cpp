#include "media_session_sink.h"

#include "../../player/media_session_thread.h"

MediaSessionPlaybackSink::MediaSessionPlaybackSink(MediaSessionThread* session)
    : session_(session) {}

bool MediaSessionPlaybackSink::tryPost(const PlaybackEvent& ev) {
    if (!session_) return true;
    const auto& snap = ev.snapshot;
    switch (ev.kind) {
    case PlaybackEvent::Kind::Started:
        session_->setPlaybackState(PlaybackState::Playing);
        // Jump-to position on resume so MPRIS clients see the correct anchor.
        session_->emitSeeked(snap.position_us);
        break;
    case PlaybackEvent::Kind::Paused:
        session_->setPlaybackState(PlaybackState::Paused);
        break;
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error:
        session_->setPlaybackState(PlaybackState::Stopped);
        break;
    case PlaybackEvent::Kind::SeekingChanged:
        session_->setSeeking(ev.flag);
        break;
    case PlaybackEvent::Kind::BufferingChanged:
        session_->setBuffering(ev.flag);
        break;
    case PlaybackEvent::Kind::TrackLoaded:
        // Pre-roll: track is loaded, mpv has not yet flipped pause=false.
        // Map to Paused so macOS/Windows NowPlaying shows the new track
        // immediately, and so MPRIS recompute picks up phase=Starting +
        // the new metadata content_ that the IPC handler already wrote.
        session_->setPlaybackState(PlaybackState::Paused);
        break;
    case PlaybackEvent::Kind::PositionChanged:
        session_->setPosition(snap.position_us);
        break;
    case PlaybackEvent::Kind::RateChanged:
        session_->setRate(snap.rate);
        break;
    case PlaybackEvent::Kind::MediaTypeChanged:
    case PlaybackEvent::Kind::DurationChanged:
    case PlaybackEvent::Kind::FullscreenChanged:
    case PlaybackEvent::Kind::OsdDimsChanged:
    case PlaybackEvent::Kind::BufferedRangesChanged:
    case PlaybackEvent::Kind::DisplayHzChanged:
        // Media metadata IPC carries the type; the rest are not media-session concerns.
        break;
    }
    return true;
}
