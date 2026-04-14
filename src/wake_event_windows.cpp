#ifdef _WIN32
#include "wake_event.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

WakeEvent::WakeEvent() {
    event_ = CreateEventW(NULL, TRUE, FALSE, NULL);  // manual-reset, initially non-signaled
}

WakeEvent::~WakeEvent() {
    if (event_) CloseHandle(event_);
}

void* WakeEvent::handle() const { return event_; }

void WakeEvent::signal() {
    SetEvent(static_cast<HANDLE>(event_));
}

void WakeEvent::drain() {
    ResetEvent(static_cast<HANDLE>(event_));
}
#endif
