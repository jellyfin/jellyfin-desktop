#include "playback_sinks.h"

#include "media_session_thread.h"

namespace {
constexpr size_t kBrowserSinkCapacity = 256;
}

bool BrowserPlaybackSink::tryPost(const PlaybackEvent& ev) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kBrowserSinkCapacity) return false;
        queue_.push_back(ev);
    }
    wake_.signal();
    return true;
}

bool BrowserPlaybackSink::drain(PlaybackEvent& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

MediaSessionPlaybackSink::MediaSessionPlaybackSink(MediaSessionThread* session)
    : session_(session) {}

bool MediaSessionPlaybackSink::tryPost(const PlaybackEvent& ev) {
    if (!session_) return true;
    switch (ev.kind) {
    case PlaybackEvent::Kind::Started:
        session_->setPlaybackState(PlaybackState::Playing);
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
    case PlaybackEvent::Kind::MediaTypeChanged:
        // Media metadata IPC carries the type; nothing to forward here.
        break;
    case PlaybackEvent::Kind::TrackLoaded:
        // Pre-roll: track is loaded, mpv has not yet flipped pause=false.
        // Map to Paused so macOS/Windows NowPlaying shows the new track
        // immediately, and so MPRIS recompute picks up phase=Starting +
        // the new metadata content_ that the IPC handler already wrote.
        session_->setPlaybackState(PlaybackState::Paused);
        break;
    }
    return true;
}
