#pragma once

#include "../event.h"

// Watches phase + media_type from ev.snapshot and updates the platform
// idle inhibit level.
class IdleInhibitSink final : public PlaybackEventSink {
public:
    bool tryPost(const PlaybackEvent& ev) override;
};
