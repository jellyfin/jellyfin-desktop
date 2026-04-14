#pragma once

// Cross-platform one-shot event for waking poll()/WaitForMultipleObjects().
// Linux: eventfd. macOS: pipe. Windows: manual-reset event.
// signal() is async-signal-safe (safe from signal handlers on POSIX).
class WakeEvent {
public:
    WakeEvent();
    ~WakeEvent();
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;

#ifdef _WIN32
    void* handle() const;  // HANDLE for WaitForMultipleObjects
#else
    int fd() const;        // readable fd for poll()
#endif
    void signal();         // wake from any thread / signal handler
    void drain();          // consume pending signals so wait blocks again
private:
#ifdef _WIN32
    void* event_ = nullptr;
#elif defined(__APPLE__)
    int pipe_[2] = {-1, -1};
#else
    int fd_ = -1;
#endif
};
