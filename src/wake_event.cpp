#include "wake_event.h"

#include "playback/jfn_wake_event.h"

WakeEvent::WakeEvent() : impl_(jfn_wake_event_new()) {}

WakeEvent::~WakeEvent() {
    jfn_wake_event_free(impl_);
}

#ifdef _WIN32
void* WakeEvent::handle() const { return jfn_wake_event_handle(impl_); }
#else
int WakeEvent::fd() const { return jfn_wake_event_fd(impl_); }
#endif

void WakeEvent::signal() { jfn_wake_event_signal(impl_); }
void WakeEvent::drain()  { jfn_wake_event_drain(impl_); }
