#include "media_session_thread.h"
#include "logging.h"
#include <chrono>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#endif

MediaSessionThread::~MediaSessionThread() {
    stop();
}

void MediaSessionThread::start(MediaSession* session) {
    session_ = session;

#if !defined(_WIN32) && !defined(__APPLE__)
    event_fd_ = eventfd(0, EFD_NONBLOCK);
    if (event_fd_ < 0) {
        LOG_ERROR(LOG_MEDIA, "eventfd creation failed");
    }
#endif

    running_.store(true);
    thread_ = std::thread(&MediaSessionThread::threadFunc, this);
    LOG_INFO(LOG_MEDIA, "media session thread started");
}

void MediaSessionThread::stop() {
    if (!running_.load()) return;

    running_.store(false);
    wake();  // Wake thread so it can exit

    if (thread_.joinable()) {
        thread_.join();
    }

#if !defined(_WIN32) && !defined(__APPLE__)
    if (event_fd_ >= 0) {
        close(event_fd_);
        event_fd_ = -1;
    }
#endif

    LOG_INFO(LOG_MEDIA, "media session thread stopped");
}

void MediaSessionThread::wake() {
#if !defined(_WIN32) && !defined(__APPLE__)
    if (event_fd_ >= 0) {
        uint64_t val = 1;
        [[maybe_unused]] auto _ = write(event_fd_, &val, sizeof(val));
    }
#else
    cv_.notify_one();
#endif
}

void MediaSessionThread::enqueue(MediaSessionCmd cmd) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(cmd));
    }
    wake();
}

void MediaSessionThread::setPlaybackState(PlaybackState state) {
    MediaSessionCmd cmd;
    cmd.type = MediaSessionCmd::Type::SetPlaybackState;
    cmd.state = state;
    enqueue(std::move(cmd));
}

void MediaSessionThread::setPosition(int64_t position_us) {
    MediaSessionCmd cmd;
    cmd.type = MediaSessionCmd::Type::SetPosition;
    cmd.position_us = position_us;
    enqueue(std::move(cmd));
}

void MediaSessionThread::setRate(double rate) {
    MediaSessionCmd cmd;
    cmd.type = MediaSessionCmd::Type::SetRate;
    cmd.rate = rate;
    enqueue(std::move(cmd));
}

void MediaSessionThread::setMetadata(const MediaMetadata& meta) {
    MediaSessionCmd cmd;
    cmd.type = MediaSessionCmd::Type::SetMetadata;
    cmd.metadata = meta;
    enqueue(std::move(cmd));
}

void MediaSessionThread::setBuffering(bool buffering) {
    MediaSessionCmd cmd;
    cmd.type = MediaSessionCmd::Type::SetBuffering;
    cmd.flag = buffering;
    enqueue(std::move(cmd));
}

void MediaSessionThread::setSeeking(bool seeking) {
    MediaSessionCmd cmd;
    cmd.type = MediaSessionCmd::Type::SetSeeking;
    cmd.flag = seeking;
    enqueue(std::move(cmd));
}

void MediaSessionThread::emitSeeked(int64_t position_us) {
    MediaSessionCmd cmd;
    cmd.type = MediaSessionCmd::Type::EmitSeeked;
    cmd.position_us = position_us;
    enqueue(std::move(cmd));
}

void MediaSessionThread::setArtwork(const std::string& url) {
    MediaSessionCmd cmd;
    cmd.type = MediaSessionCmd::Type::SetArtwork;
    cmd.artwork_url = url;
    enqueue(std::move(cmd));
}

void MediaSessionThread::setCanGoNext(bool can) {
    MediaSessionCmd cmd;
    cmd.type = MediaSessionCmd::Type::SetCanGoNext;
    cmd.flag = can;
    enqueue(std::move(cmd));
}

void MediaSessionThread::setCanGoPrevious(bool can) {
    MediaSessionCmd cmd;
    cmd.type = MediaSessionCmd::Type::SetCanGoPrevious;
    cmd.flag = can;
    enqueue(std::move(cmd));
}

void MediaSessionThread::threadFunc() {
#if !defined(_WIN32) && !defined(__APPLE__)
    // Linux: fully event-driven with poll() on D-Bus fd + eventfd
    int dbus_fd = session_->getFd();

    while (running_.load()) {
        // Process all pending commands
        std::queue<MediaSessionCmd> work;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            work.swap(queue_);
        }

        while (!work.empty()) {
            const auto& cmd = work.front();
            switch (cmd.type) {
                case MediaSessionCmd::Type::SetPlaybackState:
                    session_->setPlaybackState(cmd.state);
                    break;
                case MediaSessionCmd::Type::SetPosition:
                    session_->setPosition(cmd.position_us);
                    break;
                case MediaSessionCmd::Type::SetRate:
                    session_->setRate(cmd.rate);
                    break;
                case MediaSessionCmd::Type::SetMetadata:
                    session_->setMetadata(cmd.metadata);
                    break;
                case MediaSessionCmd::Type::SetBuffering:
                    session_->setBuffering(cmd.flag);
                    break;
                case MediaSessionCmd::Type::SetSeeking:
                    session_->setSeeking(cmd.flag);
                    break;
                case MediaSessionCmd::Type::EmitSeeked:
                    session_->emitSeeked(cmd.position_us);
                    break;
                case MediaSessionCmd::Type::SetArtwork:
                    session_->setArtwork(cmd.artwork_url);
                    break;
                case MediaSessionCmd::Type::SetCanGoNext:
                    session_->setCanGoNext(cmd.flag);
                    break;
                case MediaSessionCmd::Type::SetCanGoPrevious:
                    session_->setCanGoPrevious(cmd.flag);
                    break;
            }
            work.pop();
        }

        // Poll on D-Bus fd and eventfd
        struct pollfd fds[2];
        int nfds = 0;

        if (dbus_fd >= 0) {
            fds[nfds].fd = dbus_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        if (event_fd_ >= 0) {
            fds[nfds].fd = event_fd_;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (nfds > 0) {
            poll(fds, nfds, 100);  // 100ms timeout for shutdown check

            // Drain eventfd if signaled
            for (int i = 0; i < nfds; i++) {
                if (fds[i].fd == event_fd_ && (fds[i].revents & POLLIN)) {
                    uint64_t val;
                    [[maybe_unused]] auto _ = read(event_fd_, &val, sizeof(val));
                }
            }
        }

        // Process any incoming D-Bus messages
        session_->update();
    }
#else
    // macOS/Windows: CV-based with timeout for incoming message check
    while (running_.load()) {
        // Process all pending commands
        std::queue<MediaSessionCmd> work;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            work.swap(queue_);
        }

        while (!work.empty()) {
            const auto& cmd = work.front();
            switch (cmd.type) {
                case MediaSessionCmd::Type::SetPlaybackState:
                    session_->setPlaybackState(cmd.state);
                    break;
                case MediaSessionCmd::Type::SetPosition:
                    session_->setPosition(cmd.position_us);
                    break;
                case MediaSessionCmd::Type::SetRate:
                    session_->setRate(cmd.rate);
                    break;
                case MediaSessionCmd::Type::SetMetadata:
                    session_->setMetadata(cmd.metadata);
                    break;
                case MediaSessionCmd::Type::SetBuffering:
                    session_->setBuffering(cmd.flag);
                    break;
                case MediaSessionCmd::Type::SetSeeking:
                    session_->setSeeking(cmd.flag);
                    break;
                case MediaSessionCmd::Type::EmitSeeked:
                    session_->emitSeeked(cmd.position_us);
                    break;
                case MediaSessionCmd::Type::SetArtwork:
                    session_->setArtwork(cmd.artwork_url);
                    break;
                case MediaSessionCmd::Type::SetCanGoNext:
                    session_->setCanGoNext(cmd.flag);
                    break;
                case MediaSessionCmd::Type::SetCanGoPrevious:
                    session_->setCanGoPrevious(cmd.flag);
                    break;
            }
            work.pop();
        }

        // Check for incoming messages
        session_->update();

        // Wait for command or timeout
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(16));
    }
#endif
}
