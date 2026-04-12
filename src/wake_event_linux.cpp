#ifndef __APPLE__
#include "wake_event.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstdint>

WakeEvent::WakeEvent() {
    fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

WakeEvent::~WakeEvent() {
    if (fd_ >= 0) close(fd_);
}

int WakeEvent::fd() const { return fd_; }

void WakeEvent::signal() {
    uint64_t val = 1;
    (void)write(fd_, &val, sizeof(val));
}

void WakeEvent::drain() {
    uint64_t val;
    (void)read(fd_, &val, sizeof(val));
}
#endif
