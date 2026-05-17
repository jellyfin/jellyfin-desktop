#pragma once

#include "mpv/event.h"
#include "playback/jfn_dispatcher.h"

#include <cstring>

// mpv → coordinator bridge. mpv_digest_thread normalizes mpv events into
// MpvEvent values and calls publish(); the Rust jfn-playback dispatcher
// thread routes them into the playback coordinator. The coordinator
// delivers events to its registered sinks (BrowserPlaybackSink,
// IdleInhibitSink, ThemeColorSink, MpvActionSink) inline on its own
// worker thread.

// Set by BrowserPlaybackSink on FullscreenChanged events; read by the
// geometry-save tail in main.cpp at shutdown.
extern bool g_was_maximized_before_fullscreen;

// Convert C++ MpvEvent → JfnMpvEventC and forward to the Rust dispatcher.
inline void publish(const MpvEvent& ev) {
    JfnMpvEventC c{};
    c.type_ = static_cast<uint8_t>(ev.type);
    c.flag  = ev.flag;
    c.flag2 = ev.flag2;
    c.dbl   = ev.dbl;
    c.pw = ev.pw; c.ph = ev.ph; c.lw = ev.lw; c.lh = ev.lh;
    c.range_count = ev.range_count;
    static_assert(sizeof(BufferedRange) == sizeof(JfnDispatcherBufferedRange),
                  "BufferedRange must match JfnDispatcherBufferedRange");
    int n = ev.range_count;
    if (n > 0) {
        std::memcpy(&c.ranges[0], &ev.ranges[0],
                    static_cast<size_t>(n) * sizeof(JfnDispatcherBufferedRange));
    }
    c.err_msg = ev.err_msg;
    jfn_dispatcher_publish(&c);
}

namespace dispatcher {

inline void init() { jfn_dispatcher_init(); }
inline void shutdown() { jfn_dispatcher_shutdown(); }

inline void set_display_scale_handler(void (*cb)(double)) {
    jfn_dispatcher_set_display_scale_handler(cb);
}

inline void start() { jfn_dispatcher_start(); }
inline void stop() { jfn_dispatcher_stop(); }

}  // namespace dispatcher
