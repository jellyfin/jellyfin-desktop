#include "shutdown.h"

#include "common.h"
#include "browser/browsers.h"
#include "browser/web_browser.h"
#include "browser/overlay_browser.h"
#include "browser/about_browser.h"
#include "wake_event.h"

std::atomic<bool> g_shutting_down{false};
WakeEvent g_shutdown_event;

void initiate_shutdown() {
    bool expected = false;
    if (!g_shutting_down.compare_exchange_strong(expected, true)) return;
    try_close_browser(g_web_browser);
    try_close_browser(g_overlay_browser);
    try_close_browser(g_about_browser);
    g_shutdown_event.signal();
    // macOS main thread is parked in nextEventMatchingMask — post a sentinel
    // NSEvent so it returns and re-checks g_shutting_down.
    if (g_platform.wake_main_loop) g_platform.wake_main_loop();
}

void signal_handler(int) {
    initiate_shutdown();
}
