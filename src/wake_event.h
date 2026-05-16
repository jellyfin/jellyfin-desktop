#pragma once

struct JfnWakeEvent;

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
    JfnWakeEvent* impl_ = nullptr;
};
