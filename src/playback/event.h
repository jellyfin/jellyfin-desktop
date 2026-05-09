#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../player/media_session.h"

// Whether mpv has a player loaded. Cleared on terminal events; set on file-loaded.
enum class PlayerPresence { None, Present };

// Coarse playback phase. Distinct from MPRIS-facing PlaybackState because
// "Starting" is meaningful internally while pause flips through false → true
// during track-switch reinits, but is not exposed to consumers.
enum class PlaybackPhase { Starting, Playing, Paused, Stopped };

// Reason an mpv END_FILE event fired. Mirrors mpv's MPV_END_FILE_REASON_*.
enum class EndReason { Eof, Error, Canceled };

// Mirror of mpv's BufferedRange in coord/SM-facing form. Decoupled from
// the mpv-side fixed-size array so the SM has no mpv dependency.
struct PlaybackBufferedRange {
    int64_t start_ticks = 0;  // 100ns units
    int64_t end_ticks = 0;
};

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

    // mpv-derived state mirrored into the SM so every consumer sees one
    // coherent snapshot at event-emission time. No pull from coord.
    double rate = 1.0;
    int64_t duration_us = 0;
    bool fullscreen = false;
    bool maximized_before_fullscreen = false;
    int layout_w = 0, layout_h = 0;
    int pixel_w = 0, pixel_h = 0;
    int display_hz = 0;
    std::vector<PlaybackBufferedRange> buffered;
};

// Semantic playback events emitted by the state machine to all sinks.
// Each event carries a full snapshot of post-transition state so sinks
// never need to pull from the coordinator.
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
        PositionChanged,
        DurationChanged,
        RateChanged,
        FullscreenChanged,
        OsdDimsChanged,
        BufferedRangesChanged,
        DisplayHzChanged,
    };
    Kind kind = Kind::Started;
    bool flag = false;          // SeekingChanged/BufferingChanged value
    std::string error_message;  // Error
    PlaybackSnapshot snapshot;  // post-transition state
};

// Coord-side actions emitted by the SM alongside events. Distinct from
// events because these fire side-effecting commands (e.g. mpv async
// property writes) rather than notifying observers of state changes.
struct PlaybackAction {
    enum class Kind {
        ApplyPendingTrackSelectionAndPlay,
    };
    Kind kind;
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

class PlaybackActionSink {
public:
    virtual ~PlaybackActionSink() = default;
    virtual bool tryPost(const PlaybackAction& act) = 0;
};
