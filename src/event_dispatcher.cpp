#include "event_dispatcher.h"

bool g_was_maximized_before_fullscreen = false;

extern "C" void jfn_event_sink_pump_thunk(void* ctx) {
    auto* s = static_cast<QueuedPlaybackSink*>(ctx);
    s->wake().drain();
    s->pump();
}

extern "C" void jfn_action_sink_pump_thunk(void* ctx) {
    auto* s = static_cast<QueuedActionSink*>(ctx);
    s->wake().drain();
    s->pump();
}
