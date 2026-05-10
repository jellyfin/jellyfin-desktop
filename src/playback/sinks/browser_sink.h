#pragma once

#include "queued_sink.h"

// Forwards UI-affecting events to g_web_browser via execJs. Reads only
// from ev.snapshot — never pulls from coord.
class BrowserPlaybackSink final : public QueuedPlaybackSink {
protected:
    void deliver(const PlaybackEvent& ev) override;
};
