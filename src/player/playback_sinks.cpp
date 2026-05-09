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
        if (ev.flag) session_->emitSeeking();
        // false transitions surface via the snapshot; backends infer
        // resume from the next setPlaybackState/setPosition pair.
        break;
    case PlaybackEvent::Kind::BufferingChanged:
        session_->setBuffering(ev.flag);
        break;
    case PlaybackEvent::Kind::MediaTypeChanged:
        // Media metadata IPC carries the type; nothing to forward here.
        break;
    }
    return true;
}
