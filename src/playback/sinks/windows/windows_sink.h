#pragma once
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../queued_sink.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

// Windows SystemMediaTransportControls direct sink. Owns its own thread
// (MTA-initialized) that drives QueuedPlaybackSink::pump() on wake.
// Inbound SMTC ButtonPressed / PlaybackPositionChangeRequested handlers
// call g_mpv / g_web_browser directly.
class WindowsSink final : public QueuedPlaybackSink {
public:
    explicit WindowsSink(HWND hwnd);
    ~WindowsSink();

    void start();
    void stop();

protected:
    void deliver(const PlaybackEvent& ev) override;

private:
    struct WinRTState;

    void threadFunc();
    void initSmtc();
    void teardownSmtc();
    void updateDisplayProperties();
    void updateTimeline();
    void onButtonPressed(int button);

    HWND hwnd_;
    std::unique_ptr<WinRTState> state_;

    MediaMetadata metadata_;
    PlaybackState playback_state_ = PlaybackState::Stopped;
    int64_t position_us_ = 0;
    bool pending_update_ = false;
    std::chrono::steady_clock::time_point last_position_update_;

    std::thread thread_;
    std::atomic<bool> running_{false};
};

#endif // _WIN32
