#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JfnWakeEvent JfnWakeEvent;

// Allocate a one-shot wake event. Returns NULL on failure.
// Linux: eventfd. macOS: pipe. Windows: manual-reset event.
JfnWakeEvent* jfn_wake_event_new(void);

void jfn_wake_event_free(JfnWakeEvent* ev);

// Async-signal-safe on POSIX (write of 1-8 bytes). On Windows SetEvent is
// callable from any thread.
void jfn_wake_event_signal(const JfnWakeEvent* ev);

// Consume pending signals so the next wait blocks again.
void jfn_wake_event_drain(const JfnWakeEvent* ev);

#ifndef _WIN32
// Readable fd for poll(). Returns -1 on a NULL handle.
int jfn_wake_event_fd(const JfnWakeEvent* ev);
#else
// HANDLE for WaitForMultipleObjects. Returns NULL on a NULL handle.
void* jfn_wake_event_handle(const JfnWakeEvent* ev);
#endif

#ifdef __cplusplus
}
#endif
