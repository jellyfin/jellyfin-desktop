#pragma once

#ifndef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JfnSignalGuard JfnSignalGuard;

// Snapshot SIGINT/SIGTERM dispositions and install `handler` on both.
// Returns an opaque handle; pass to jfn_signal_guard_free() to restore the
// prior dispositions. Returns NULL on allocation failure.
JfnSignalGuard* jfn_signal_guard_install(void (*handler)(int));

// Restore the snapshotted SIGINT/SIGTERM dispositions and release the
// handle. Safe to call with NULL.
void jfn_signal_guard_free(JfnSignalGuard* guard);

#ifdef __cplusplus
}
#endif

#endif  // !_WIN32
