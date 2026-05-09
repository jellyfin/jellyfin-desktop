#include "playback_state_machine.h"

#include <utility>

namespace {

bool is_active_phase(PlaybackPhase p) {
    return p == PlaybackPhase::Starting
        || p == PlaybackPhase::Playing
        || p == PlaybackPhase::Paused;
}

}  // namespace

std::vector<PlaybackEvent> PlaybackStateMachine::onFileLoaded() {
    s_.presence = PlayerPresence::Present;
    s_.phase = PlaybackPhase::Starting;
    s_.seeking = false;
    s_.buffering = false;
    s_.position_us = 0;
    return {};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onPauseChanged(bool paused) {
    if (s_.presence == PlayerPresence::None) return {};
    if (s_.phase == PlaybackPhase::Stopped) return {};

    if (paused) {
        if (s_.phase == PlaybackPhase::Paused) return {};
        s_.phase = PlaybackPhase::Paused;
        return {{PlaybackEvent::Kind::Paused}};
    }

    // pause=false: leave Starting/Paused, become Playing. Self-edge silent.
    if (s_.phase == PlaybackPhase::Playing) return {};
    s_.phase = PlaybackPhase::Playing;
    return {{PlaybackEvent::Kind::Started}};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onEndFile(
    EndReason reason, std::string error_message)
{
    std::vector<PlaybackEvent> out;

    if (s_.seeking) {
        s_.seeking = false;
        PlaybackEvent e{PlaybackEvent::Kind::SeekingChanged};
        e.flag = false;
        out.push_back(e);
    }
    if (s_.buffering) {
        s_.buffering = false;
        PlaybackEvent e{PlaybackEvent::Kind::BufferingChanged};
        e.flag = false;
        out.push_back(e);
    }

    s_.presence = PlayerPresence::None;
    s_.phase = PlaybackPhase::Stopped;
    s_.position_us = 0;

    PlaybackEvent terminal;
    switch (reason) {
    case EndReason::Eof:      terminal.kind = PlaybackEvent::Kind::Finished; break;
    case EndReason::Canceled: terminal.kind = PlaybackEvent::Kind::Canceled; break;
    case EndReason::Error:
        terminal.kind = PlaybackEvent::Kind::Error;
        terminal.error_message = std::move(error_message);
        break;
    }
    out.push_back(std::move(terminal));
    return out;
}

std::vector<PlaybackEvent> PlaybackStateMachine::onSeekingChanged(bool seeking) {
    if (!is_active_phase(s_.phase)) {
        // No player → ignore. Snapshot already has seeking=false from terminal.
        return {};
    }
    if (s_.seeking == seeking) return {};
    s_.seeking = seeking;
    PlaybackEvent e{PlaybackEvent::Kind::SeekingChanged};
    e.flag = seeking;
    return {e};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onBufferingChanged(bool buffering) {
    if (!is_active_phase(s_.phase)) return {};
    if (s_.buffering == buffering) return {};
    s_.buffering = buffering;
    PlaybackEvent e{PlaybackEvent::Kind::BufferingChanged};
    e.flag = buffering;
    return {e};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onPosition(int64_t position_us) {
    s_.position_us = position_us;
    if (s_.seeking) {
        s_.seeking = false;
        PlaybackEvent e{PlaybackEvent::Kind::SeekingChanged};
        e.flag = false;
        return {e};
    }
    return {};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onMediaType(MediaType type) {
    if (s_.media_type == type) return {};
    s_.media_type = type;
    PlaybackEvent e{PlaybackEvent::Kind::MediaTypeChanged};
    e.media_type = type;
    return {e};
}
