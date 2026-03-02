#include "cef_thread.h"
#include "cef_app.h"
#include "logging.h"
#include "include/cef_task.h"
#include <cstdlib>
#include <future>

CefThread::~CefThread() {
    shutdown();
}

bool CefThread::start(CefMainArgs& args, CefSettings& settings, CefRefPtr<CefApp> app) {
    // Use CEF's internal message loop (not external_message_pump)
    settings.external_message_pump = false;
    settings.multi_threaded_message_loop = false;

    thread_ = std::thread(&CefThread::threadFunc, this, args, settings, app);

    // Wait for init to complete
    std::unique_lock<std::mutex> lock(init_mutex_);
    init_cv_.wait(lock, [this] { return init_complete_; });

    return init_success_;
}

namespace {
class QuitTask : public CefTask {
public:
    void Execute() override { CefQuitMessageLoop(); }
    IMPLEMENT_REFCOUNTING(QuitTask);
};
}

void CefThread::shutdown() {
    if (running_.load()) {
        // Post quit to CEF's UI thread (must be called from same thread as CefInitialize)
        try {
            CefPostTask(TID_UI, new QuitTask());
        } catch (...) {
            LOG_ERROR(LOG_CEF, "Failed to post quit task to CEF thread");
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void CefThread::threadFunc(CefMainArgs args, CefSettings settings, CefRefPtr<CefApp> app) {
    LOG_INFO(LOG_CEF, "CEF thread starting");

    // Initialize CEF on this thread
    bool success = CefInitialize(args, settings, app, nullptr);

    if (!success) {
        LOG_ERROR(LOG_CEF, "CefInitialize failed on CEF thread");
        std::lock_guard<std::mutex> lock(init_mutex_);
        init_success_ = false;
        init_complete_ = true;
        init_cv_.notify_one();
        return;
    }

    running_.store(true);
    LOG_INFO(LOG_CEF, "CEF thread running");

    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        init_success_ = true;
        init_complete_ = true;
    }
    init_cv_.notify_one();

    // Run CEF's internal message loop - blocks until CefQuitMessageLoop() is called
    CefRunMessageLoop();

    LOG_INFO(LOG_CEF, "CEF thread: CefRunMessageLoop returned, calling CefShutdown...");

    // CefShutdown() can hang waiting for subprocess IPC cleanup.
    // Run it with a timeout to avoid blocking the process indefinitely.
    auto future = std::async(std::launch::async, []() { CefShutdown(); });
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
        LOG_WARN(LOG_CEF, "CEF thread: CefShutdown timed out after 5s, forcing exit");
        _exit(0);
    }

    LOG_INFO(LOG_CEF, "CEF thread: CefShutdown complete");
    running_.store(false);
}
