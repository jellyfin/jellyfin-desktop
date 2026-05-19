#include "shutdown.h"

#include "common.h"
#include "browser/browsers.h"

std::atomic<bool> g_shutting_down{false};
JfnWakeEvent* g_shutdown_event = jfn_wake_event_new();

void initiate_shutdown() {
    bool expected = false;
    if (!g_shutting_down.compare_exchange_strong(expected, true)) return;
    if (g_browsers) g_browsers->closeAll();
    jfn_wake_event_signal(g_shutdown_event);
    // macOS main thread is parked in nextEventMatchingMask — post a sentinel
    // NSEvent so it returns and re-checks g_shutting_down.
    if (g_platform.wake_main_loop) g_platform.wake_main_loop();
}

void signal_handler(int) {
    initiate_shutdown();
}

// Exposed to the Rust-side CefLayer port (src/jfn_cef/src/client.rs). Used
// to gate posted-task work that must not race with CefShutdown teardown.
extern "C" bool jfn_shutting_down() {
    return g_shutting_down.load(std::memory_order_relaxed);
}
