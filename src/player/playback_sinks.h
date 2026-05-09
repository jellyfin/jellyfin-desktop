#pragma once

#include "playback_event.h"
#include "../wake_event.h"

#include <deque>
#include <mutex>

class MediaSessionThread;

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

// Forwards UI-affecting events to g_web_browser via execJs. Reads only
// from ev.snapshot — never pulls from coord.
class BrowserPlaybackSink final : public CefThreadPlaybackSink {
protected:
    void deliver(const PlaybackEvent& ev) override;
};

// Watches phase + media_type from ev.snapshot and updates the platform
// idle inhibit level.
class IdleInhibitSink final : public CefThreadPlaybackSink {
protected:
    void deliver(const PlaybackEvent& ev) override;
};

// Resets ThemeColor video mode on terminal events. (Active-true setVideoMode
// fires from web_browser.cpp on metadata arrival; that's not mpv-derived
// and stays out of the playback event stream.)
class ThemeColorSink final : public CefThreadPlaybackSink {
protected:
    void deliver(const PlaybackEvent& ev) override;
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

// Runs g_mpv.ApplyPendingTrackSelectionAndPlay() on cef_consumer_thread
// in response to ApplyPendingTrackSelectionAndPlay actions emitted by
// the SM on FILE_LOADED. Preserves the prior ordering relative to the
// FILE_LOADED drain.
class MpvActionSink final : public CefThreadActionSink {
protected:
    void deliver(const PlaybackAction& act) override;
};
