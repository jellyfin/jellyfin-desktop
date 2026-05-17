#include "shutdown.h"

#include "common.h"
#include "browser/browsers.h"
#include "wake_event.h"

std::atomic<bool> g_shutting_down{false};
WakeEvent g_shutdown_event;
bool g_was_maximized_before_fullscreen = false;

void initiate_shutdown() {
    bool expected = false;
    if (!g_shutting_down.compare_exchange_strong(expected, true)) return;
    if (g_browsers) g_browsers->closeAll();
    g_shutdown_event.signal();
    // macOS main thread is parked in nextEventMatchingMask — post a sentinel
    // NSEvent so it returns and re-checks g_shutting_down.
    if (g_platform.wake_main_loop) g_platform.wake_main_loop();
}

void signal_handler(int) {
    initiate_shutdown();
}
