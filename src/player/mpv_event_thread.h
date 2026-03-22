#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#include <string>

class MpvPlayer;

// Event types that mpv thread queues for main thread
struct MpvEvent {
    enum class Type {
        Position,
        Duration,
        Speed,
        Playing,
        Paused,
        Finished,
        Canceled,
        Seeking,
        Seeked,
        Buffering,
        CoreIdle,
        BufferedRanges,
        Error
    };

    Type type;
    double value = 0;           // position/duration in ms
    bool flag = false;          // paused/buffering/idle
    std::string error;          // error message
    std::vector<std::pair<int64_t, int64_t>> ranges;  // buffered ranges
};

// Runs mpv event processing on dedicated thread
class MpvEventThread {
public:
    MpvEventThread() = default;
    ~MpvEventThread();

    // Start thread - takes ownership of event processing
    void start(MpvPlayer* player);

    // Stop thread
    void stop();

    // Main thread calls this to get pending events (swaps buffers)
    std::vector<MpvEvent> drain();

private:
    void threadFunc();
    void wake();

    MpvPlayer* player_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> woken_{false};

    std::mutex mutex_;
    std::vector<MpvEvent> pending_;

    std::mutex cv_mutex_;
    std::condition_variable cv_;
};
