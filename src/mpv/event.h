#pragma once

#include <mpv/client.h>
#include <cstdint>

enum class MpvEventType {
    NONE,       // sentinel -- unhandled, don't publish
    SHUTDOWN,
    FILE_LOADED,
    END_FILE_EOF,
    END_FILE_ERROR,
    END_FILE_CANCEL,
    PAUSE,
    TIME_POS,
    DURATION,
    FULLSCREEN,
    OSD_DIMS,
    SPEED,
    SEEKING,
    DISPLAY_FPS,
    BUFFERED_RANGES,
};

static constexpr int MAX_BUFFERED_RANGES = 8;

struct BufferedRange {
    int64_t start_ticks;    // 100ns units (ticks)
    int64_t end_ticks;
};

struct MpvEvent {
    MpvEventType type;
    bool flag;              // PAUSE, FULLSCREEN, SEEKING
    double dbl;             // TIME_POS, DURATION, SPEED
    int pw, ph, lw, lh;    // OSD_DIMS
    int range_count;                            // BUFFERED_RANGES
    BufferedRange ranges[MAX_BUFFERED_RANGES];  // BUFFERED_RANGES
};

// Observation IDs passed as reply_userdata to mpv_observe_property.
// digest_property uses these to switch instead of string-comparing names.
enum MpvObserveId : uint64_t {
    MPV_OBSERVE_VIDEO_PARAMS  = 1,
    MPV_OBSERVE_OSD_DIMS      = 2,
    MPV_OBSERVE_FULLSCREEN    = 3,
    MPV_OBSERVE_PAUSE         = 4,
    MPV_OBSERVE_TIME_POS      = 5,
    MPV_OBSERVE_DURATION      = 6,
    MPV_OBSERVE_SPEED         = 7,
    MPV_OBSERVE_SEEKING       = 8,
    MPV_OBSERVE_DISPLAY_FPS   = 9,
    MPV_OBSERVE_CACHE_STATE   = 10,
};

class MpvHandle;

void observe_properties(MpvHandle& mpv);
MpvEvent digest_property(uint64_t id, mpv_event_property* p);
