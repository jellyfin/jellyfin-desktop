#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors BufferedRange in src/mpv/event.h.
typedef struct {
    int64_t start_ticks;
    int64_t end_ticks;
} JfnDispatcherBufferedRange;

// Flat C struct used to publish mpv events. type_ values must match
// MpvEventType in src/mpv/event.h. err_msg, when non-null, is borrowed for
// the duration of jfn_dispatcher_publish (mpv's static error strings
// satisfy this trivially).
typedef struct {
    uint8_t type_;
    bool    flag;
    bool    flag2;  // FULLSCREEN: was_maximized
    double  dbl;
    int32_t pw;
    int32_t ph;
    int32_t lw;
    int32_t lh;
    int32_t range_count;
    JfnDispatcherBufferedRange ranges[8];
    const char* err_msg;
} JfnMpvEventC;

void jfn_dispatcher_init(void);
void jfn_dispatcher_shutdown(void);

// Install the browsers.setScale handler used to resolve DISPLAY_SCALE
// events. The C++ side passes a thunk that calls g_browsers->setScale.
void jfn_dispatcher_set_display_scale_handler(void (*cb)(double));

// Push one event onto the queue. Safe from any thread.
void jfn_dispatcher_publish(const JfnMpvEventC* ev);

// Spawn the consumer thread. Call after all sinks/handlers are registered.
void jfn_dispatcher_start(void);

// Signal shutdown and join the consumer thread.
void jfn_dispatcher_stop(void);

#ifdef __cplusplus
}
#endif
