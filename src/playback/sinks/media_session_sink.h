#pragma once

#include "../event.h"

class MediaSessionThread;

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
