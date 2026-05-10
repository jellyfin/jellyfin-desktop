#pragma once

#include "../event.h"
#include "../../wake_event.h"

#include <deque>
#include <mutex>

// Base for sinks that hand events off to a consumer thread. Coordinator-side
// tryPost() enqueues + signals wake; some consumer thread polls wake().fd()
// (or platform equivalent), drains via pump(), which calls each subclass's
// deliver(). Backed by a bounded deque; phase-transition / terminal events
// fit comfortably inside the capacity.
//
// Who drives pump() is up to the sink: existing UI-side subclasses are
// pumped by the cef_consumer_thread via event_dispatcher; platform
// media-session sinks own their own thread and call pump() from inside
// their own run loop.
class QueuedPlaybackSink : public PlaybackEventSink {
public:
    bool tryPost(const PlaybackEvent& ev) override;

    // Drains every queued event and invokes deliver() on each.
    void pump();

    WakeEvent& wake() { return wake_; }

protected:
    virtual void deliver(const PlaybackEvent& ev) = 0;

private:
    std::mutex mutex_;
    std::deque<PlaybackEvent> queue_;
    WakeEvent wake_;
};

// Base for action sinks. Same shape as QueuedPlaybackSink for actions.
class QueuedActionSink : public PlaybackActionSink {
public:
    bool tryPost(const PlaybackAction& act) override;

    void pump();

    WakeEvent& wake() { return wake_; }

protected:
    virtual void deliver(const PlaybackAction& act) = 0;

private:
    std::mutex mutex_;
    std::deque<PlaybackAction> queue_;
    WakeEvent wake_;
};
