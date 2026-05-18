#pragma once

#include "../queued_sink.h"

#include <atomic>
#include <chrono>
#include <thread>

// macOS Now Playing direct sink. Owns its own thread that drives
// QueuedPlaybackSink::pump() on wake. Inbound MPRemoteCommandCenter
// handlers call g_mpv / g_web_browser directly.
class MacosSink final : public QueuedPlaybackSink {
public:
    MacosSink();
    ~MacosSink();

    void start();
    void stop();

protected:
    void deliver(const PlaybackEvent& ev) override;

private:
    void threadFunc();
    void initRemote();
    void teardownRemote();
    void updateNowPlayingInfo();
    void updateTimelineThrottled(int64_t position_us, bool force);

    void* delegate_ = nullptr;            // MediaKeysDelegate (Obj-C, __bridge_retained)
    void* media_remote_lib_ = nullptr;    // dlopen handle for MediaRemote.framework

    MediaMetadata metadata_;
    int64_t position_us_ = 0;
    double rate_ = 1.0;
    std::chrono::steady_clock::time_point last_position_update_;

    typedef void (*SetNowPlayingVisibilityFunc)(void* origin, int visibility);
    typedef void* (*GetLocalOriginFunc)(void);
    typedef void (*SetCanBeNowPlayingApplicationFunc)(int);
    SetNowPlayingVisibilityFunc SetNowPlayingVisibility_ = nullptr;
    GetLocalOriginFunc GetLocalOrigin_ = nullptr;
    SetCanBeNowPlayingApplicationFunc SetCanBeNowPlayingApplication_ = nullptr;

    std::thread thread_;
    std::atomic<bool> running_{false};
};
