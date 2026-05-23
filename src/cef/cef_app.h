#pragma once

// CEF process bootstrap. The implementation lives in the jfn-cef Rust crate;
// this header just declares the C ABI it exports so main.cpp / platform code
// can call it directly.
//
// CEF re-execs this binary to spawn GPU/renderer/utility children. Chromium
// delivers --type=... and related switches to those children via argv, so
// children must receive the full argv. The initial (browser) process must
// NOT forward the user's shell argv into CEF — any Chromium switch we want
// there goes through OnBeforeCommandLineProcessing. Parent/child is
// discriminated via an inherited env var.

extern "C" {

// Call once at the top of main(), after platform early_init. If this process
// is a CEF-spawned subprocess, it runs to completion; the return value is
// the exit code the caller must `return` from main. If this is the initial
// (browser) process, returns -1 and startup should continue.
int  jfn_cef_start(int argc, char* argv[]);

// Configuration for the not-yet-started browser process. Call between
// jfn_cef_start() and jfn_cef_initialize(). Values are applied when CEF
// asks us for them.
void jfn_cef_set_log_severity(int severity);
void jfn_cef_set_remote_debugging_port(int port);
void jfn_cef_set_disable_gpu_compositing(bool disable);
void jfn_cef_set_ignore_certificate_errors(bool ignore);
#ifdef __linux__
void jfn_cef_set_ozone_platform(const char* platform_utf8);
#endif

// Builds CefSettings (paths, locale, message pump, sandbox, cache dir, etc.),
// performs any platform pre-init (e.g. macOS message pump source/timer), and
// calls CefInitialize for the browser process. Returns true on success.
bool jfn_cef_initialize();

// Tears down CEF for the browser process. Call once during shutdown after
// all browsers have closed.
void jfn_cef_shutdown();

}  // extern "C"

// CEF runtime lifetime.
class CefRuntimeScope {
public:
    CefRuntimeScope() : ok_(jfn_cef_initialize()) {}
    ~CefRuntimeScope() { if (ok_) jfn_cef_shutdown(); }
    bool ok() const { return ok_; }

    CefRuntimeScope(const CefRuntimeScope&) = delete;
    CefRuntimeScope& operator=(const CefRuntimeScope&) = delete;
private:
    bool ok_;
};
