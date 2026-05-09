#include "coordinator.h"

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

void PlaybackCoordinator::addActionSink(std::shared_ptr<PlaybackActionSink> sink) {
    action_sinks_.push_back(std::move(sink));
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

void PlaybackCoordinator::postLoadStarting(std::string item_id) {
    Input in{Input::Kind::LoadStarting};
    in.str = std::move(item_id);
    enqueue(std::move(in));
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

void PlaybackCoordinator::postPausedForCache(bool pfc) {
    Input in{Input::Kind::PausedForCache};
    in.flag = pfc;
    enqueue(std::move(in));
}

void PlaybackCoordinator::postCoreIdle(bool core_idle) {
    Input in{Input::Kind::CoreIdle};
    in.flag = core_idle;
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

void PlaybackCoordinator::postVideoFrameAvailable(bool available) {
    Input in{Input::Kind::VideoFrameAvailable};
    in.flag = available;
    enqueue(std::move(in));
}

void PlaybackCoordinator::postSpeed(double rate) {
    Input in{Input::Kind::Speed};
    in.dbl = rate;
    enqueue(std::move(in));
}

void PlaybackCoordinator::postDuration(int64_t duration_us) {
    Input in{Input::Kind::Duration};
    in.i64 = duration_us;
    enqueue(std::move(in));
}

void PlaybackCoordinator::postFullscreen(bool fullscreen, bool was_maximized) {
    Input in{Input::Kind::Fullscreen};
    in.flag = fullscreen;
    in.flag2 = was_maximized;
    enqueue(std::move(in));
}

void PlaybackCoordinator::postOsdDims(int lw, int lh, int pw, int ph) {
    Input in{Input::Kind::OsdDims};
    in.lw = lw; in.lh = lh; in.pw = pw; in.ph = ph;
    enqueue(std::move(in));
}

void PlaybackCoordinator::postBufferedRanges(std::vector<PlaybackBufferedRange> ranges) {
    Input in{Input::Kind::BufferedRanges};
    in.ranges = std::move(ranges);
    enqueue(std::move(in));
}

void PlaybackCoordinator::postDisplayHz(int hz) {
    Input in{Input::Kind::DisplayHz};
    in.hz = hz;
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
    case Input::Kind::LoadStarting:
        emitted = sm_.onLoadStarting(in.str);
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
    case Input::Kind::PausedForCache:
        emitted = sm_.onPausedForCache(in.flag);
        break;
    case Input::Kind::CoreIdle:
        emitted = sm_.onCoreIdle(in.flag);
        break;
    case Input::Kind::Position:
        emitted = sm_.onPosition(in.i64);
        break;
    case Input::Kind::MediaType:
        emitted = sm_.onMediaType(in.media_type);
        break;
    case Input::Kind::VideoFrameAvailable:
        emitted = sm_.onVideoFrameAvailable(in.flag);
        break;
    case Input::Kind::Speed:
        emitted = sm_.onSpeed(in.dbl);
        break;
    case Input::Kind::Duration:
        emitted = sm_.onDuration(in.i64);
        break;
    case Input::Kind::Fullscreen:
        emitted = sm_.onFullscreen(in.flag, in.flag2);
        break;
    case Input::Kind::OsdDims:
        emitted = sm_.onOsdDims(in.lw, in.lh, in.pw, in.ph);
        break;
    case Input::Kind::BufferedRanges:
        emitted = sm_.onBufferedRanges(in.ranges);
        break;
    case Input::Kind::DisplayHz:
        emitted = sm_.onDisplayHz(in.hz);
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
        std::vector<PlaybackAction> actions;
        for (const auto& in : work) {
            apply(in, events);
            auto produced = sm_.consumeActions();
            for (auto& a : produced) actions.push_back(std::move(a));
        }

        // Stamp every event with the live snapshot so sinks never pull
        // from coord. Snapshot is the post-transition value of all
        // events emitted in this batch (sinks treat each event as the
        // edge that produced this state).
        PlaybackSnapshot snap = sm_.snapshot();
        for (auto& e : events) e.snapshot = snap;

        // Publish snapshot under its own lock for non-event readers
        // (hotkeys, idle inhibit reads outside the event path).
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_ = snap;
        }

        // Sinks deliver via their own executor; tryPost must not block.
        // Order is preserved because emitted events are appended in
        // SM-emission order and we walk sinks in registration order.
        for (auto& sink : sinks_) {
            for (const auto& e : events) {
                (void)sink->tryPost(e);
            }
        }
        for (auto& sink : action_sinks_) {
            for (const auto& a : actions) {
                (void)sink->tryPost(a);
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
