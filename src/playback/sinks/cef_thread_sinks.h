#pragma once

#include "../event.h"
#include "../../wake_event.h"

#include <deque>
#include <mutex>

// Base for sinks that handle PlaybackEvents on the cef_consumer_thread.
// Coordinator-side tryPost() enqueues + signals wake; cef_consumer_thread
// polls wake().fd(), drains via pump(), which calls each subclass's
// deliver(). Backed by a bounded deque; phase-transition / terminal
// events fit comfortably inside the capacity.
class CefThreadPlaybackSink : public PlaybackEventSink {
public:
    bool tryPost(const PlaybackEvent& ev) override;

    // Called by cef_consumer_thread after wake fires. Drains every
    // queued event and invokes deliver() on each.
    void pump();

    WakeEvent& wake() { return wake_; }

protected:
    virtual void deliver(const PlaybackEvent& ev) = 0;

private:
    std::mutex mutex_;
    std::deque<PlaybackEvent> queue_;
    WakeEvent wake_;
};

// Base for action sinks that run on cef_consumer_thread.
class CefThreadActionSink : public PlaybackActionSink {
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
