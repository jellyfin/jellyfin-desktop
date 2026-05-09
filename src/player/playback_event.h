#pragma once

#include <cstdint>
#include <string>

#include "media_session.h"

// Whether mpv has a player loaded. Cleared on terminal events; set on file-loaded.
enum class PlayerPresence { None, Present };

// Coarse playback phase. Distinct from MPRIS-facing PlaybackState because
// "Starting" is meaningful internally while pause flips through false → true
// during track-switch reinits, but is not exposed to consumers.
enum class PlaybackPhase { Starting, Playing, Paused, Stopped };

// Reason an mpv END_FILE event fired. Mirrors mpv's MPV_END_FILE_REASON_*.
enum class EndReason { Eof, Error, Canceled };

struct PlaybackSnapshot {
    PlayerPresence presence = PlayerPresence::None;
    PlaybackPhase phase = PlaybackPhase::Stopped;
    bool seeking = false;
    bool buffering = false;
    MediaType media_type = MediaType::Unknown;
    int64_t position_us = 0;
    // True between a load-starting hint whose Jellyfin item Id matches
    // the previous load's Id (bitrate change, transcode-audio change,
    // any same-item reload) and the next FILE_LOADED. Lets consumers
    // distinguish "user is reloading the same item" from a fresh track
    // change without re-deriving identity.
    bool variant_switch_pending = false;
};

// Semantic playback events emitted by the state machine to all sinks.
// SeekingChanged/BufferingChanged carry the new flag value.
// MediaTypeChanged carries the new media_type. Error carries a message.
struct PlaybackEvent {
    enum class Kind {
        Started,
        Paused,
        Finished,
        Canceled,
        Error,
        SeekingChanged,
        BufferingChanged,
        MediaTypeChanged,
        TrackLoaded,
    };
    Kind kind = Kind::Started;
    bool flag = false;
    MediaType media_type = MediaType::Unknown;
    std::string error_message;
};

// Narrow non-blocking interface. Coordinator calls tryPost from the
// coordinator worker thread; sinks must enqueue/handoff and return
// immediately. Returning false signals a full queue (sink is responsible
// for its own coalescing/drop policy as documented per sink).
class PlaybackEventSink {
public:
    virtual ~PlaybackEventSink() = default;
    virtual bool tryPost(const PlaybackEvent& ev) = 0;
};
