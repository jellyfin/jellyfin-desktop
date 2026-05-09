#pragma once

#include "cef_thread_sinks.h"

// Watches phase + media_type from ev.snapshot and updates the platform
// idle inhibit level.
class IdleInhibitSink final : public CefThreadPlaybackSink {
protected:
    void deliver(const PlaybackEvent& ev) override;
};
