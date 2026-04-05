#pragma once

// Cross-platform one-shot event for waking poll().
// Linux: eventfd. macOS: pipe.
// signal() is async-signal-safe (safe from signal handlers).
class WakeEvent {
public:
    WakeEvent();
    ~WakeEvent();
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;

    int fd() const;   // readable fd for poll()
    void signal();    // write from any thread / signal handler
    void drain();     // consume pending signals so poll() blocks again
private:
#ifdef __APPLE__
    int pipe_[2] = {-1, -1};
#else
    int fd_ = -1;
#endif
};
