#include "queued_sink.h"

namespace {
constexpr size_t kEventSinkCapacity = 256;
constexpr size_t kActionSinkCapacity = 64;
}  // namespace

bool QueuedPlaybackSink::tryPost(const PlaybackEvent& ev) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kEventSinkCapacity) return false;
        queue_.push_back(ev);
    }
    wake_.signal();
    return true;
}

void QueuedPlaybackSink::pump() {
    std::deque<PlaybackEvent> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(queue_);
    }
    for (const auto& ev : drained) deliver(ev);
}

bool QueuedActionSink::tryPost(const PlaybackAction& act) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kActionSinkCapacity) return false;
        queue_.push_back(act);
    }
    wake_.signal();
    return true;
}

void QueuedActionSink::pump() {
    std::deque<PlaybackAction> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(queue_);
    }
    for (const auto& a : drained) deliver(a);
}
