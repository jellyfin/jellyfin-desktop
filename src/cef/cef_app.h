#pragma once

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <functional>

class App : public CefApp,
            public CefBrowserProcessHandler,
            public CefRenderProcessHandler {
public:
    App() = default;

    // Set device scale factor before CefInitialize
    void SetDeviceScaleFactor(float scale) { device_scale_factor_ = scale; }
    void SetDisableGpuCompositing(bool v) { disable_gpu_compositing_ = v; }

    // Set wake callback for external_message_pump mode (macOS/Linux)
    // Must be called before CefInitialize
    static void SetWakeCallback(std::function<void()> callback) { wake_callback_ = std::move(callback); }

    // External message pump interface (macOS/Linux)
    // Call when wake event received - pumps CEF work
    static void DoWork();

    // Main loop sleep coordination: call setWaiting(true) before SDL_WaitEvent,
    // then check hasWorkPending() — if true, skip the wait.
    static bool hasWorkPending() { return work_pending_.load(std::memory_order_seq_cst); }
    static void setWaiting(bool v) { is_waiting_.store(v, std::memory_order_seq_cst); }

    // CefApp
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override;
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

    // CefBrowserProcessHandler
    void OnContextInitialized() override;
    void OnScheduleMessagePumpWork(int64_t delay_ms) override;
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

    // CefRenderProcessHandler
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;

private:
    // External message pump state (macOS/Linux)
    static inline std::function<void()> wake_callback_;
    static inline std::atomic<bool> is_active_{false};  // Re-entrancy guard
    static inline std::atomic<bool> work_pending_{false};  // Immediate work pending
    static inline std::atomic<bool> is_waiting_{false};  // True while main loop is in SDL_WaitEvent
    static inline SDL_TimerID timer_id_{0};  // For delayed work
    static Uint32 TimerCallback(void* userdata, SDL_TimerID id, Uint32 interval);

    float device_scale_factor_ = 1.0f;
    bool disable_gpu_compositing_ = false;

    IMPLEMENT_REFCOUNTING(App);
    DISALLOW_COPY_AND_ASSIGN(App);
};

// V8 handler for native functions
class NativeV8Handler : public CefV8Handler {
public:
    NativeV8Handler(CefRefPtr<CefBrowser> browser) : browser_(browser) {}

    bool Execute(const CefString& name,
                CefRefPtr<CefV8Value> object,
                const CefV8ValueList& arguments,
                CefRefPtr<CefV8Value>& retval,
                CefString& exception) override;

private:
    CefRefPtr<CefBrowser> browser_;
    IMPLEMENT_REFCOUNTING(NativeV8Handler);
};
