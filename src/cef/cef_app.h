#pragma once

#include "include/internal/cef_types.h"

#include <string>

// CEF process bootstrap. Encapsulates the multi-process dance so main.cpp
// doesn't need to know about library loaders, MainArgs, subprocess dispatch,
// argv sanitization, or the App object.
//
// CEF re-execs this binary to spawn GPU/renderer/utility children. Chromium
// delivers --type=... and related switches to those children via argv, so
// children must receive the full argv. The initial (browser) process must
// NOT forward the user's shell argv into CEF — any Chromium switch we want
// there goes through OnBeforeCommandLineProcessing. Parent/child is
// discriminated via an inherited env var.
namespace CefRuntime {

// Call once at the top of main(), after platform early_init. If this process
// is a CEF-spawned subprocess, it runs to completion; the return value is
// the exit code the caller must `return` from main. If this is the initial
// (browser) process, returns -1 and startup should continue.
int Start(int argc, char* argv[]);

// Configuration for the not-yet-started browser process. Call between
// Start() and Initialize(). Values are applied when CEF asks us for them.
void SetLogSeverity(cef_log_severity_t severity);
void SetRemoteDebuggingPort(int port);            // 0 = disabled
void SetDisableGpuCompositing(bool disable);
#ifdef __linux__
void SetOzonePlatform(const std::string& platform);
#endif

// Builds CefSettings (paths, locale, message pump, sandbox, cache dir, etc.),
// performs any platform pre-init (e.g. macOS message pump source/timer), and
// calls CefInitialize for the browser process. Returns true on success.
bool Initialize();

// Tears down CEF for the browser process. Call once during shutdown after
// all browsers have closed.
void Shutdown();

}  // namespace CefRuntime
