#pragma once

#include "playback_event.h"
#include "../wake_event.h"

#include <deque>
#include <mutex>

class MediaSessionThread;

// Sink that hands playback events off to the cef_consumer_thread queue.
// cef_consumer_thread is responsible for: execJs IPC to the main browser,
// idle-inhibit refresh, and theme-color reset on terminal events.
//
// Backed by a bounded deque + WakeEvent. tryPost is non-blocking (mutex
// held only for the push). Phase-transition and terminal events are
// not lossy: capacity is generous and producers post one event per
// transition. SeekingChanged/BufferingChanged are coalesce-safe by
// pulling the live snapshot at delivery time.
class BrowserPlaybackSink final : public PlaybackEventSink {
public:
    bool tryPost(const PlaybackEvent& ev) override;

    // cef_consumer_thread integration.
    bool drain(PlaybackEvent& out);
    WakeEvent& wake() { return wake_; }

private:
    std::mutex mutex_;
    std::deque<PlaybackEvent> queue_;
    WakeEvent wake_;
};

// Sink that forwards playback events to the platform media session via
// the existing MediaSessionThread (already mutex-queue + own thread).
// tryPost calls thread-safe enqueue methods that copy a small command;
// it never touches D-Bus / MediaRemote directly.
class MediaSessionPlaybackSink final : public PlaybackEventSink {
public:
    explicit MediaSessionPlaybackSink(MediaSessionThread* session);
    bool tryPost(const PlaybackEvent& ev) override;
private:
    MediaSessionThread* session_;
};
