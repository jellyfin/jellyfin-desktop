#include "idle_inhibit_sink.h"

#include "../../platform/platform.h"

namespace {
void apply_idle_inhibit(const PlaybackSnapshot& snap) {
    if (snap.phase != PlaybackPhase::Playing) {
        g_platform.set_idle_inhibit(IdleInhibitLevel::None);
    } else if (snap.media_type == MediaType::Audio) {
        g_platform.set_idle_inhibit(IdleInhibitLevel::System);
    } else {
        g_platform.set_idle_inhibit(IdleInhibitLevel::Display);
    }
}
}  // namespace

void IdleInhibitSink::deliver(const PlaybackEvent& ev) {
    switch (ev.kind) {
    case PlaybackEvent::Kind::Started:
    case PlaybackEvent::Kind::Paused:
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error:
    case PlaybackEvent::Kind::MediaTypeChanged:
        apply_idle_inhibit(ev.snapshot);
        break;
    default:
        break;
    }
}
