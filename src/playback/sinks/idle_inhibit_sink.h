#pragma once

#include "queued_sink.h"

// Watches phase + media_type from ev.snapshot and updates the platform
// idle inhibit level.
class IdleInhibitSink final : public QueuedPlaybackSink {
protected:
    void deliver(const PlaybackEvent& ev) override;
};
