#pragma once

#include "playback_event.h"

#include <cstdint>
#include <string>
#include <vector>

// Pure deterministic state machine. No threads, globals, platform calls,
// CEF calls, media-session calls, or logging. All state lives in the
// returned snapshot. Inputs return the list of semantic events emitted
// by the transition (often empty).
class PlaybackStateMachine {
public:
    PlaybackSnapshot snapshot() const { return s_; }

    // mpv MPV_EVENT_FILE_LOADED. Enters Present + Starting. Forces
    // seeking/buffering off. Does not emit Started/Paused — those wait
    // for the first valid PAUSE flip.
    std::vector<PlaybackEvent> onFileLoaded();

    // mpv 'pause' property change. Pause events while presence==None or
    // phase==Stopped are silently ignored — mpv's pause flag is
    // observable while idle and pause=false there does not mean playback.
    std::vector<PlaybackEvent> onPauseChanged(bool paused);

    // mpv MPV_EVENT_END_FILE. Force-clears seeking/buffering, transitions
    // to None + Stopped, emits a single terminal event matching reason.
    std::vector<PlaybackEvent> onEndFile(EndReason reason,
                                         std::string error_message = {});

    // mpv 'seeking' property change. Self-edges silent.
    std::vector<PlaybackEvent> onSeekingChanged(bool seeking);

    // mpv 'paused-for-cache' property change. Self-edges silent.
    std::vector<PlaybackEvent> onBufferingChanged(bool buffering);

    // mpv 'time-pos' update. Updates snapshot position; if seeking is
    // active, the first position update completes the seek (clears the
    // flag and emits SeekingChanged(false)).
    std::vector<PlaybackEvent> onPosition(int64_t position_us);

    // Browser-driven media-type change (Audio/Video/Unknown).
    std::vector<PlaybackEvent> onMediaType(MediaType type);

private:
    PlaybackSnapshot s_;
};
