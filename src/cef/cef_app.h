#pragma once

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"

class App : public CefApp,
            public CefBrowserProcessHandler,
            public CefRenderProcessHandler {
public:
    App() = default;

    void SetDisableGpuCompositing(bool v) { disable_gpu_compositing_ = v; }
    void SetOzonePlatform(const std::string& p) { ozone_platform_ = p; }

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

#ifdef __APPLE__
    // external_message_pump support (macOS only). InitPump() installs a
    // CFRunLoopSource and CFRunLoopTimer in the main runloop's common modes;
    // OnScheduleMessagePumpWork signals the source (immediate) or sets the
    // timer's next fire date (delayed). Both are serviced by [NSApp run]'s
    // CFRunLoopRun loop. Must be called once after [NSApplication
    // sharedApplication] and before CefInitialize. Call ShutdownPump() after
    // the post-run CEF drain completes (and before CefShutdown) to invalidate
    // the source/timer and gate any racing wakes.
    static void InitPump();
    static void ShutdownPump();
#endif

    // CefRenderProcessHandler
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;

private:
    bool disable_gpu_compositing_ = false;
    std::string ozone_platform_;

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
