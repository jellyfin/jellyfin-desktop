#include "cef_thread_sinks.h"

namespace {
constexpr size_t kEventSinkCapacity = 256;
constexpr size_t kActionSinkCapacity = 64;
}  // namespace

bool CefThreadPlaybackSink::tryPost(const PlaybackEvent& ev) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kEventSinkCapacity) return false;
        queue_.push_back(ev);
    }
    wake_.signal();
    return true;
}

void CefThreadPlaybackSink::pump() {
    std::deque<PlaybackEvent> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(queue_);
    }
    for (const auto& ev : drained) deliver(ev);
}

bool CefThreadActionSink::tryPost(const PlaybackAction& act) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kActionSinkCapacity) return false;
        queue_.push_back(act);
    }
    wake_.signal();
    return true;
}

void CefThreadActionSink::pump() {
    std::deque<PlaybackAction> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(queue_);
    }
    for (const auto& a : drained) deliver(a);
}
