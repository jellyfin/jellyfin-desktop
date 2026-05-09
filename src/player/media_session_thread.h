#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <cstdint>
#include <condition_variable>
#include "media_session.h"

// Commands that can be sent to media session thread
struct MediaSessionCmd {
    enum class Type {
        SetPlaybackState,
        SetPosition,
        SetRate,
        SetMetadata,
        SetBuffering,
        SetSeeking,
        EmitSeeked,
        SetArtwork,
        SetCanGoNext,
        SetCanGoPrevious
    };

    Type type;
    PlaybackState state = PlaybackState::Stopped;
    int64_t position_us = 0;
    double rate = 1.0;
    MediaMetadata metadata;
    std::string artwork_url;
    bool flag = false;  // for canGoNext/canGoPrevious
};

// Runs media session updates on dedicated thread
class MediaSessionThread {
public:
    MediaSessionThread() = default;
    ~MediaSessionThread();

    void start(MediaSession* session);
    void stop();

    // Queue commands (non-blocking)
    void setPlaybackState(PlaybackState state);
    void setPosition(int64_t position_us);
    void setRate(double rate);
    void setMetadata(const MediaMetadata& meta);
    void setBuffering(bool buffering);
    void setSeeking(bool seeking);
    void emitSeeked(int64_t position_us);
    void setArtwork(const std::string& url);
    void setCanGoNext(bool can);
    void setCanGoPrevious(bool can);

private:
    void threadFunc();
    void enqueue(MediaSessionCmd cmd);
    void wake();  // Wake thread to process commands

    MediaSession* session_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex mutex_;
    std::queue<MediaSessionCmd> queue_;
    std::condition_variable cv_;  // For non-Linux platforms

#if !defined(_WIN32) && !defined(__APPLE__)
    int event_fd_ = -1;  // eventfd for Linux poll() wakeup
#endif
};
