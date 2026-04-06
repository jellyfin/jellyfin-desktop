#pragma once

#include <atomic>
#include <cstddef>
#include "wake_event.h"

// Lock-free SPSC (single-producer, single-consumer) ring buffer.
// Producer: try_push (non-blocking, signals wake event on success).
// Consumer: blocks on wake().fd(), drains with try_pop.
template<typename T, size_t N = 256>
class EventQueue {
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
public:
    bool try_push(const T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) & (N - 1);
        if (next == tail_.load(std::memory_order_acquire))
            return false;  // full
        buf_[head] = item;
        head_.store(next, std::memory_order_release);
        wake_.signal();
        return true;
    }

    bool try_pop(T& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;  // empty
        item = buf_[tail];
        tail_.store((tail + 1) & (N - 1), std::memory_order_release);
        return true;
    }

    WakeEvent& wake() { return wake_; }

#ifdef _WIN32
    void* wake_handle() { return wake_.handle(); }
#endif

    // Drain the wake event so poll()/WaitForMultipleObjects blocks on next call.
    // Call after draining the queue, before re-entering wait.
    void drain_wake() { wake_.drain(); }

private:
    T buf_[N];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
    WakeEvent wake_;
};
