#pragma once

#ifndef _WIN32
#include <signal.h>

// Snapshots SIGINT/SIGTERM handlers, installs the supplied handler, and
// restores the prior handlers in the destructor — mirrors cef_app.cpp's
// snapshot/restore pattern around CefInitialize so we leave the process'
// signal disposition the way we found it on every exit path.
class SignalHandlerGuard {
public:
    explicit SignalHandlerGuard(void (*handler)(int)) {
        struct sigaction sa{};
        sa.sa_handler = handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT,  &sa, &prev_int_);
        sigaction(SIGTERM, &sa, &prev_term_);
    }
    ~SignalHandlerGuard() {
        sigaction(SIGINT,  &prev_int_,  nullptr);
        sigaction(SIGTERM, &prev_term_, nullptr);
    }
    SignalHandlerGuard(const SignalHandlerGuard&) = delete;
    SignalHandlerGuard& operator=(const SignalHandlerGuard&) = delete;
private:
    struct sigaction prev_int_{};
    struct sigaction prev_term_{};
};
#endif
