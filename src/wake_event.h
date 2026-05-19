#pragma once

#include "playback/jfn_wake_event.h"

// Cross-platform one-shot event for waking poll()/WaitForMultipleObjects().
// Linux: eventfd. macOS: pipe. Windows: manual-reset event.
// signal() is async-signal-safe (safe from signal handlers on POSIX).
// Thin C++ wrapper over the Rust jfn_wake_event_* FFI.
class WakeEvent {
public:
    WakeEvent() : impl_(jfn_wake_event_new()) {}
    ~WakeEvent() { jfn_wake_event_free(impl_); }
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;

#ifdef _WIN32
    void* handle() const { return jfn_wake_event_handle(impl_); }
#else
    int fd() const { return jfn_wake_event_fd(impl_); }
#endif
    void signal() { jfn_wake_event_signal(impl_); }
    void drain()  { jfn_wake_event_drain(impl_); }

private:
    JfnWakeEvent* impl_ = nullptr;
};
