#include "mpv_event_thread.h"
#include "mpv/mpv_player.h"
#include "logging.h"

MpvEventThread::~MpvEventThread() {
    stop();
}

void MpvEventThread::start(MpvPlayer* player) {
    player_ = player;

    // Set up callbacks that queue events instead of executing directly
    player_->setPositionCallback([this](double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::Position, ms});
    });

    player_->setDurationCallback([this](double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::Duration, ms});
    });

    player_->setSpeedCallback([this](double speed) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::Speed, speed});
    });

    player_->setPlayingCallback([this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::Playing});
    });

    player_->setStateCallback([this](bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::Paused, 0, paused});
    });

    player_->setFinishedCallback([this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::Finished});
    });

    player_->setCanceledCallback([this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::Canceled});
    });

    player_->setSeekingCallback([this](double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::Seeking, ms});
    });

    player_->setSeekedCallback([this](double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::Seeked, ms});
    });

    player_->setBufferingCallback([this](bool buffering, double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::Buffering, ms, buffering});
    });

    player_->setCoreIdleCallback([this](bool idle, double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(MpvEvent{MpvEvent::Type::CoreIdle, ms, idle});
    });

    player_->setBufferedRangesCallback([this](const std::vector<MpvPlayer::BufferedRange>& ranges) {
        std::lock_guard<std::mutex> lock(mutex_);
        MpvEvent ev;
        ev.type = MpvEvent::Type::BufferedRanges;
        for (const auto& r : ranges) {
            ev.ranges.emplace_back(r.start, r.end);
        }
        pending_.push_back(std::move(ev));
    });

    player_->setErrorCallback([this](const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        MpvEvent ev;
        ev.type = MpvEvent::Type::Error;
        ev.error = error;
        pending_.push_back(std::move(ev));
    });

    // Set wakeup callback to notify our CV when mpv has events
    player_->setWakeupCallback([this]() { wake(); });

    running_.store(true);
    thread_ = std::thread(&MpvEventThread::threadFunc, this);
    LOG_INFO(LOG_MPV, "mpv event thread started");
}

void MpvEventThread::stop() {
    if (!running_.load()) return;
    running_.store(false);
    cv_.notify_one();  // Wake thread so it can exit
    if (thread_.joinable()) {
        thread_.join();
    }
    LOG_INFO(LOG_MPV, "mpv event thread stopped");
}

std::vector<MpvEvent> MpvEventThread::drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MpvEvent> result;
    result.swap(pending_);
    return result;
}

void MpvEventThread::wake() {
    woken_.store(true);
    cv_.notify_one();
}

void MpvEventThread::threadFunc() {
    while (running_.load()) {
        woken_.store(false);
        player_->processEvents();

        // Wait for mpv wakeup callback or shutdown
        std::unique_lock lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return !running_.load() || woken_.load();
        });
    }
}
