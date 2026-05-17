#include "coordinator.h"

#include "../common.h"

std::atomic<bool> g_playback_coord_running{false};

extern "C" bool jfn_event_sink_thunk(void* ctx, const JfnPlaybackEventC* ev) {
    auto* sink = static_cast<PlaybackEventSink*>(ctx);
    PlaybackEvent e = playback_ffi::from_c(*ev);
    return sink->tryPost(e);
}

extern "C" bool jfn_action_sink_thunk(void* ctx, const JfnPlaybackActionC* act) {
    auto* sink = static_cast<PlaybackActionSink*>(ctx);
    PlaybackAction a;
    a.kind = static_cast<PlaybackAction::Kind>(act->kind);
    return sink->tryPost(a);
}

PlaybackCoordinatorScope::PlaybackCoordinatorScope() {
    playback::init();
    g_playback_coord_running.store(true, std::memory_order_release);
}

PlaybackCoordinatorScope::~PlaybackCoordinatorScope() {
    g_playback_coord_running.store(false, std::memory_order_release);
    playback::shutdown();
}
