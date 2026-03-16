#pragma once

#include <SDL3/SDL.h>

struct wl_display;

// Reliable cross-thread event loop wakeup for Linux.
//
// SDL_WaitEvent on Linux uses poll() on only the display fd, so
// SDL_PushEvent from worker threads can't wake it.  This class adds
// an eventfd that poll() also watches, and exposes wake()/waitForEvent()
// so the main loop doesn't need to know the details.
class EventLoopWake {
public:
    // Detects Wayland/X11 display fd from the SDL window and creates
    // the eventfd.  Safe to call even if both fail (falls back to
    // SDL_WaitEvent internally).
    void init(SDL_Window* window);

    // Writes to the eventfd to unblock waitForEvent().
    // Thread-safe — designed to be called from CEF threads, timer
    // callbacks, etc.  Safe to call before init() (no-op).
    void wake();

    // Blocks until an SDL event is available (or the eventfd fires).
    // Returns true and fills |event| if an event was dequeued.
    bool waitForEvent(SDL_Event* event);

    // Closes the eventfd.
    void cleanup();

private:
    int wake_fd_ = -1;
    int display_fd_ = -1;
    struct wl_display* wl_display_ = nullptr;
};
