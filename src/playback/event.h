#pragma once

#include "jfn_playback.h"

#include <cstdint>
#include <string>
#include <vector>

// C++-side data types used by sinks. The coordinator + state machine live
// in the Rust `jfn-playback` crate; these structs mirror the FFI shapes
// in jfn_playback.h with std::string / std::vector for C++ ergonomics.

enum class MediaType : uint8_t {
    Unknown = 0,
    Audio = 1,
    Video = 2,
};

enum class PlayerPresence : uint8_t {
    None = 0,
    Present = 1,
};

enum class PlaybackPhase : uint8_t {
    Starting = 0,
    Playing = 1,
    Paused = 2,
    Stopped = 3,
};

enum class EndReason : uint8_t {
    Eof = 0,
    Error = 1,
    Canceled = 2,
};

struct MediaMetadata {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    int track_number = 0;
    int64_t duration_us = 0;
    std::string art_url;
    std::string art_data_uri;
    MediaType media_type = MediaType::Unknown;

    bool operator==(const MediaMetadata& o) const {
        return id == o.id && title == o.title && artist == o.artist
            && album == o.album && track_number == o.track_number
            && duration_us == o.duration_us && art_url == o.art_url
            && art_data_uri == o.art_data_uri && media_type == o.media_type;
    }
};

struct PlaybackBufferedRange {
    int64_t start_ticks = 0;
    int64_t end_ticks = 0;
};

struct PlaybackSnapshot {
    PlayerPresence presence = PlayerPresence::None;
    PlaybackPhase phase = PlaybackPhase::Stopped;
    bool seeking = false;
    bool buffering = false;
    MediaType media_type = MediaType::Unknown;
    int64_t position_us = 0;
    bool variant_switch_pending = false;
    double rate = 1.0;
    int64_t duration_us = 0;
    bool fullscreen = false;
    bool maximized_before_fullscreen = false;
    int layout_w = 0, layout_h = 0;
    int pixel_w = 0, pixel_h = 0;
    double display_hz = 0.0;
    std::vector<PlaybackBufferedRange> buffered;
};

struct PlaybackEvent {
    enum class Kind : uint8_t {
        Started = 0,
        Paused = 1,
        Finished = 2,
        Canceled = 3,
        Error = 4,
        SeekingChanged = 5,
        BufferingChanged = 6,
        MediaTypeChanged = 7,
        TrackLoaded = 8,
        PositionChanged = 9,
        DurationChanged = 10,
        RateChanged = 11,
        FullscreenChanged = 12,
        OsdDimsChanged = 13,
        BufferedRangesChanged = 14,
        DisplayHzChanged = 15,
        MetadataChanged = 16,
        ArtworkChanged = 17,
        QueueCapsChanged = 18,
        Seeked = 19,
    };
    Kind kind = Kind::Started;
    bool flag = false;
    std::string error_message;
    PlaybackSnapshot snapshot;
    MediaMetadata metadata;
    std::string artwork_uri;
    bool can_go_next = false;
    bool can_go_prev = false;
};

struct PlaybackAction {
    enum class Kind : uint8_t {
        ApplyPendingTrackSelectionAndPlay = 0,
    };
    Kind kind = Kind::ApplyPendingTrackSelectionAndPlay;
};

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
