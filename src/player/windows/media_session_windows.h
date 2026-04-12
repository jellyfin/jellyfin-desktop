#pragma once
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "player/media_session.h"
#include <chrono>

class WindowsMediaBackend : public MediaSessionBackend {
public:
    explicit WindowsMediaBackend(MediaSession* session, HWND hwnd);
    ~WindowsMediaBackend() override;

    void setMetadata(const MediaMetadata& meta) override;
    void setArtwork(const std::string& dataUri) override;
    void setPlaybackState(PlaybackState state) override;
    void setPosition(int64_t position_us) override;
    void setVolume(double volume) override;
    void setCanGoNext(bool can) override;
    void setCanGoPrevious(bool can) override;
    void setRate(double rate) override;
    void emitSeeked(int64_t position_us) override;
    void update() override {}   // No-op: WinRT events are callback-driven
    int getFd() override { return -1; }

    MediaSession* session() { return session_; }

private:
    struct WinRTState;

    void updateDisplayProperties();
    void updateTimeline();
    void onButtonPressed(int button);

    MediaSession* session_;
    std::unique_ptr<WinRTState> state_;

    MediaMetadata metadata_;
    PlaybackState playback_state_ = PlaybackState::Stopped;
    int64_t position_us_ = 0;
    bool pending_update_ = false;
    std::chrono::steady_clock::time_point last_position_update_;
};

std::unique_ptr<MediaSessionBackend> createWindowsMediaBackend(
    MediaSession* session, HWND hwnd);

#endif // _WIN32
