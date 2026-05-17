#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JfnClipboardWayland JfnClipboardWayland;

// Callback fired on the clipboard worker thread when a read completes.
// `text` is NOT NUL-terminated and `len` may be 0. The pointer is only valid
// for the duration of the call.
typedef void (*JfnClipboardReadCb)(void* ctx, const char* text, size_t len);

// Open a dedicated wl_display connection, bind ext-data-control-v1, start
// a worker thread. Returns NULL on any failure (no Wayland session, no
// ext-data-control-v1 in the compositor, no seat, etc.).
JfnClipboardWayland* jfn_clipboard_wayland_init(void);

// Start an async read of the current CLIPBOARD selection as UTF-8 text.
// `cb` will be invoked on the worker thread with the text, or an empty
// buffer if nothing text-shaped is on the clipboard. Safe to call from any
// thread. If `cb` is NULL, the request is dropped silently.
void jfn_clipboard_wayland_read_text_async(
    JfnClipboardWayland* c, JfnClipboardReadCb cb, void* ctx);

// Join the worker thread and destroy clipboard Wayland objects.
void jfn_clipboard_wayland_cleanup(JfnClipboardWayland* c);

#ifdef __cplusplus
}
#endif
