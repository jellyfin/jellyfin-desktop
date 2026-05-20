#pragma once

#include <stdbool.h>

#include "playback/jfn_wake_event.h"

#ifdef __cplusplus
extern "C" {
#endif

// Process-wide shutdown signal. Owned by the jfn-playback Rust crate.
//
// `jfn_shutdown_initiate` is idempotent. The first call sets the flag,
// signals the shutdown wake event, and runs the C-side handler registered
// via `jfn_shutdown_set_handler` (typically: close all CEF browsers + post
// a main-loop sentinel so a parked event loop wakes up).
//
// `jfn_shutting_down` is a relaxed atomic read suitable for hot polling.
//
// `jfn_shutdown_event` returns a JfnWakeEvent* valid for the remainder of
// the process; threads parked in poll()/WaitForMultipleObjects observe its
// fd/handle to detect shutdown.

bool jfn_shutting_down(void);
const JfnWakeEvent* jfn_shutdown_event(void);
void jfn_shutdown_initiate(void);
void jfn_shutdown_set_handler(void (*handler)(void));

#ifdef __cplusplus
}
#endif
