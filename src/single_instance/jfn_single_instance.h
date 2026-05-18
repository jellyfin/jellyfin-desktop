#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Callback invoked from the listener thread when a "raise" message arrives
// from another instance. `token` is a NUL-terminated UTF-8 string with the
// XDG activation token (empty on Windows or when unavailable).
typedef void (*JfnSingleInstanceRaiseCb)(const char* token, void* userdata);

// Try to signal an already-running instance to raise its window.
// Returns 1 if an existing instance was found and signaled, 0 otherwise.
int jfn_single_instance_try_signal_existing(void);

// Start listening for signals from future instances. Returns 1 on success,
// 0 on error (callback will not be invoked). Idempotent — calling twice
// without stopListener in between is a no-op returning 1.
int jfn_single_instance_start_listener(JfnSingleInstanceRaiseCb cb, void* userdata);

// Stop the listener thread and release the socket/pipe.
void jfn_single_instance_stop_listener(void);

#ifdef __cplusplus
}
#endif
