#pragma once

#include "include/cef_app.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

// Runs CEF on a dedicated thread using CefRunMessageLoop
class CefThread {
public:
    CefThread() = default;
    ~CefThread();

    // Start CEF thread - blocks until CefInitialize completes
    // Returns false if CefInitialize failed
    bool start(CefMainArgs& args, CefSettings& settings, CefRefPtr<CefApp> app);

    // Signal shutdown and wait for thread to finish
    void shutdown();

    // Check if running
    bool isRunning() const { return running_.load(); }

private:
    void threadFunc(CefMainArgs args, const CefSettings& settings, CefRefPtr<CefApp> app);

    std::thread thread_;
    std::atomic<bool> running_{false};

    // Init synchronization
    std::mutex init_mutex_;
    std::condition_variable init_cv_;
    bool init_complete_ = false;
    bool init_success_ = false;
};
