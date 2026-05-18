#pragma once

#ifndef _WIN32

#include "signal_guard/jfn_signal_guard.h"

// RAII wrapper around jfn-signal-guard. Snapshots SIGINT/SIGTERM
// dispositions on construction and restores them on destruction so we
// leave the process' signal disposition the way we found it on every
// exit path (mirrors the snapshot/restore pattern cef_app.cpp uses
// around CefInitialize).
class SignalHandlerGuard {
public:
    explicit SignalHandlerGuard(void (*handler)(int))
        : g_(jfn_signal_guard_install(handler)) {}
    ~SignalHandlerGuard() { jfn_signal_guard_free(g_); }
    SignalHandlerGuard(const SignalHandlerGuard&) = delete;
    SignalHandlerGuard& operator=(const SignalHandlerGuard&) = delete;
private:
    JfnSignalGuard* g_;
};

#endif
