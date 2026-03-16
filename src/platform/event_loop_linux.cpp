#include "platform/event_loop_linux.h"
#include "logging.h"
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <X11/Xlib.h>

void EventLoopWake::init(SDL_Window* window) {
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0)
        LOG_WARN(LOG_MAIN, "eventfd() failed: %s — event loop may not wake reliably",
                 strerror(errno));

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    const char* videoDriver = SDL_GetCurrentVideoDriver();
    if (videoDriver && strcmp(videoDriver, "wayland") == 0) {
        wl_display_ = static_cast<struct wl_display*>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
        if (wl_display_) display_fd_ = wl_display_get_fd(wl_display_);
    } else {
        auto* x11_display = static_cast<Display*>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        if (x11_display) display_fd_ = XConnectionNumber(x11_display);
    }
    LOG_INFO(LOG_MAIN, "Event loop display_fd=%d (%s)", display_fd_,
             videoDriver ? videoDriver : "unknown");
}

void EventLoopWake::wake() {
    if (wake_fd_ >= 0) {
        uint64_t val = 1;
        (void)write(wake_fd_, &val, sizeof(val));
    }
}

bool EventLoopWake::waitForEvent(SDL_Event* event) {
    SDL_PumpEvents();
    if (SDL_PollEvent(event))
        return true;

    struct pollfd fds[2];
    int nfds = 0;
    int wake_slot = -1;
    if (display_fd_ >= 0)
        fds[nfds++] = {display_fd_, POLLIN, 0};
    if (wake_fd_ >= 0) {
        wake_slot = nfds;
        fds[nfds++] = {wake_fd_, POLLIN, 0};
    }

    if (nfds > 0) {
        if (wl_display_) wl_display_flush(wl_display_);
        poll(fds, nfds, -1);
    } else {
        // Both fds unavailable — fall back to SDL_WaitEvent
        return SDL_WaitEvent(event);
    }

    // Drain eventfd only if it actually fired
    if (wake_slot >= 0 && (fds[wake_slot].revents & POLLIN)) {
        uint64_t val;
        (void)read(wake_fd_, &val, sizeof(val));
    }

    SDL_PumpEvents();
    return SDL_PollEvent(event);
}

void EventLoopWake::cleanup() {
    if (wake_fd_ >= 0) {
        close(wake_fd_);
        wake_fd_ = -1;
    }
}
