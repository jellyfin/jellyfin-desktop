// Minimal entry point. Almost all logic lives in jfn-rust (see
// src/jfn_rust/src/app.rs). main.cpp keeps only:
//   * the C++ main() that calls into Rust jfn_app_main,
//   * the macOS-specific mpv terminate dance (mpv's VO uninit does
//     DispatchQueue.main.sync, so TerminateDestroy must run off the main
//     thread while the CFRunLoop is pumped here),
//   * the post_window_cleanup invocation.
// Slice 1f will delete this file entirely once the macOS teardown moves
// to Rust.

#include "platform/platform.h"
#include "platform/display_backend.h"
#include "mpv/jfn_mpv_boot.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <signal.h>
#include <atomic>
#include <thread>
#include <limits>
#endif

extern "C" mpv_handle* jfn_mpv_handle_get(void);
extern "C" void jfn_mpv_handle_terminate(void);

// Rust-side entry points (src/jfn_rust/src/app.rs).
extern "C" int jfn_app_main(int argc, const char* const* argv);
extern "C" void jfn_app_teardown(void);
extern "C" void jfn_app_vo_size(int* w, int* h);
extern "C" int jfn_app_run_with_cef(int mw, int mh);
extern "C" void jfn_g_platform_post_window_cleanup(void);

int main(int argc, char* argv[]) {
    // Linux platform selection happens inside jfn_app_main after CLI
    // parsing. Windows/macOS use a fixed backend; populate g_platform
    // here so the Rust subprocess-dispatch path (CefExecuteProcess) has
    // the right vtable available immediately.
#ifdef _WIN32
    g_platform = make_platform(DisplayBackend::Windows);
#elif defined(__APPLE__)
    g_platform = make_platform(DisplayBackend::macOS);
#endif

    if (int rc = jfn_app_main(argc, const_cast<const char* const*>(argv)); rc >= 0)
        return rc;

    int mw = 0, mh = 0;
    jfn_app_vo_size(&mw, &mh);
    if (int rc = jfn_app_run_with_cef(mw, mh); rc != 0)
        return rc;

#ifdef __APPLE__
    // mpv's VO uninit (mac_common.swift:84) does DispatchQueue.main.sync
    // to close its window — calling TerminateDestroy from the main
    // thread would deadlock. Run it on a side thread and pump the
    // runloop here (same pattern as Chromium's MessagePumpCFRunLoop::DoRun).
    std::atomic<bool> mpv_done{false};
    std::thread mpv_teardown([&mpv_done]{
        // CefInitialize reset SIGALRM to SIG_DFL (content_main.cc:108);
        // mpv's PreciseTimer.terminate() sends pthread_kill(SIGALRM), so
        // restore a no-op handler before tearing down the timer.
        signal(SIGALRM, [](int){});
        jfn_mpv_handle_terminate();
        mpv_done.store(true, std::memory_order_release);
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
    while (!mpv_done.load(std::memory_order_acquire))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,
                           std::numeric_limits<CFTimeInterval>::max(), true);
    mpv_teardown.join();
#else
    jfn_mpv_handle_terminate();
#endif

    jfn_app_teardown();
    jfn_g_platform_post_window_cleanup();
    return 0;
}
