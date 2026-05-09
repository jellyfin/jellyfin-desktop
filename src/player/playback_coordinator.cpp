#include "playback_coordinator.h"

#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#endif

PlaybackCoordinator::PlaybackCoordinator() = default;

PlaybackCoordinator::~PlaybackCoordinator() {
    stop();
}

void PlaybackCoordinator::addSink(std::shared_ptr<PlaybackEventSink> sink) {
    sinks_.push_back(std::move(sink));
}

void PlaybackCoordinator::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&PlaybackCoordinator::worker, this);
}

void PlaybackCoordinator::stop() {
    if (!running_.exchange(false)) return;
    wake_.signal();
    if (thread_.joinable()) thread_.join();
}

void PlaybackCoordinator::enqueue(Input in) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(std::move(in));
    }
    wake_.signal();
}

void PlaybackCoordinator::postFileLoaded() {
    enqueue({Input::Kind::FileLoaded});
}

void PlaybackCoordinator::postPauseChanged(bool paused) {
    Input in{Input::Kind::PauseChanged};
    in.flag = paused;
    enqueue(std::move(in));
}

void PlaybackCoordinator::postEndFile(EndReason reason, std::string error_message) {
    Input in{Input::Kind::EndFile};
    in.reason = reason;
    in.str = std::move(error_message);
    enqueue(std::move(in));
}

void PlaybackCoordinator::postSeekingChanged(bool seeking) {
    Input in{Input::Kind::SeekingChanged};
    in.flag = seeking;
    enqueue(std::move(in));
}

void PlaybackCoordinator::postBufferingChanged(bool buffering) {
    Input in{Input::Kind::BufferingChanged};
    in.flag = buffering;
    enqueue(std::move(in));
}

void PlaybackCoordinator::postPosition(int64_t position_us) {
    Input in{Input::Kind::Position};
    in.i64 = position_us;
    enqueue(std::move(in));
}

void PlaybackCoordinator::postMediaType(MediaType type) {
    Input in{Input::Kind::MediaType};
    in.media_type = type;
    enqueue(std::move(in));
}

PlaybackSnapshot PlaybackCoordinator::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
}

void PlaybackCoordinator::apply(const Input& in, std::vector<PlaybackEvent>& out) {
    std::vector<PlaybackEvent> emitted;
    switch (in.kind) {
    case Input::Kind::FileLoaded:
        emitted = sm_.onFileLoaded();
        break;
    case Input::Kind::PauseChanged:
        emitted = sm_.onPauseChanged(in.flag);
        break;
    case Input::Kind::EndFile:
        emitted = sm_.onEndFile(in.reason, in.str);
        break;
    case Input::Kind::SeekingChanged:
        emitted = sm_.onSeekingChanged(in.flag);
        break;
    case Input::Kind::BufferingChanged:
        emitted = sm_.onBufferingChanged(in.flag);
        break;
    case Input::Kind::Position:
        emitted = sm_.onPosition(in.i64);
        break;
    case Input::Kind::MediaType:
        emitted = sm_.onMediaType(in.media_type);
        break;
    }
    for (auto& e : emitted) out.push_back(std::move(e));
}

void PlaybackCoordinator::worker() {
    while (running_.load(std::memory_order_relaxed)) {
        std::deque<Input> work;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            work.swap(queue_);
        }

        if (work.empty()) {
            // Block until either a new input arrives or stop() signals.
            // Loop check above re-evaluates running_ on wake.
            // POSIX: read drains the eventfd/pipe; Windows: manual reset.
            // The poll/wait happens inside WakeEvent's drain via a separate
            // wait primitive — but our WakeEvent doesn't expose blocking
            // wait directly, so do a one-fd poll here.
#ifdef _WIN32
            void* h = wake_.handle();
            WaitForSingleObject(h, INFINITE);
#else
            struct pollfd pfd{wake_.fd(), POLLIN, 0};
            poll(&pfd, 1, -1);
#endif
            wake_.drain();
            continue;
        }

        std::vector<PlaybackEvent> events;
        for (const auto& in : work) apply(in, events);

        // Publish snapshot under its own lock; readers must observe
        // post-transition state.
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_ = sm_.snapshot();
        }

        // Sinks deliver via their own executor; tryPost must not block.
        // Order is preserved because emitted events are appended in
        // SM-emission order and we walk sinks in registration order.
        for (auto& sink : sinks_) {
            for (const auto& e : events) {
                (void)sink->tryPost(e);
            }
        }
    }
}

PlaybackCoordinatorScope::PlaybackCoordinatorScope() {
    coord_.start();
}

PlaybackCoordinatorScope::~PlaybackCoordinatorScope() {
    coord_.stop();
}
