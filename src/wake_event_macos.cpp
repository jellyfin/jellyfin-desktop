#ifdef __APPLE__
#include "wake_event.h"
#include <unistd.h>
#include <fcntl.h>

WakeEvent::WakeEvent() {
    pipe(pipe_);
    fcntl(pipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_[1], F_SETFL, O_NONBLOCK);
    fcntl(pipe_[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipe_[1], F_SETFD, FD_CLOEXEC);
}

WakeEvent::~WakeEvent() {
    close(pipe_[0]);
    close(pipe_[1]);
}

int WakeEvent::fd() const { return pipe_[0]; }

void WakeEvent::signal() {
    char c = 1;
    (void)write(pipe_[1], &c, 1);
}

void WakeEvent::drain() {
    char buf[64];
    while (read(pipe_[0], buf, sizeof(buf)) > 0) {}
}
#endif
