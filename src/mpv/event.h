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
    const char* err_msg;    // END_FILE_ERROR — points to mpv's static error string
};

// Observation IDs passed as reply_userdata to mpv_observe_property.
// digest_property uses these to switch instead of string-comparing names.
enum MpvObserveId : uint64_t {
    MPV_OBSERVE_OSD_DIMS      = 2,
    MPV_OBSERVE_FULLSCREEN    = 3,
    MPV_OBSERVE_PAUSE         = 4,
    MPV_OBSERVE_TIME_POS      = 5,
    MPV_OBSERVE_DURATION      = 6,
    MPV_OBSERVE_SPEED         = 7,
    MPV_OBSERVE_SEEKING       = 8,
    MPV_OBSERVE_DISPLAY_FPS   = 9,
    MPV_OBSERVE_CACHE_STATE   = 10,
    MPV_OBSERVE_WINDOW_MAX    = 11,
    MPV_OBSERVE_DISPLAY_SCALE = 12,
};

class MpvHandle;

void observe_properties(MpvHandle& mpv);
MpvEvent digest_property(uint64_t id, mpv_event_property* p);

namespace mpv {
    bool fullscreen();
    bool window_maximized();
    int  osd_pw();
    int  osd_ph();

    // Effective window pixel size — the dimensions we asked mpv for. Set
    // during boot geometry resolution (and any runtime resize we initiate);
    // never overwritten by osd-dimensions events, so it survives cases
    // where osd-dims hasn't caught up to a resize we just issued.
    // Returns 0 before the first call to set_window_pixels.
    int  window_pw();
    int  window_ph();
    void set_window_pixels(int pw, int ph);
    // Cached value of mpv's display-hidpi-scale, updated from property
    // observation. Returns 0 before the first event arrives; callers
    // should treat 0 as "not yet known" and fall back to 1.0.
    double display_scale();

    // Read osd-dimensions 'w' and 'h' from an MPV_EVENT_PROPERTY_CHANGE
    // payload (MPV_FORMAT_NODE / NODE_MAP, per mpv's mp_property_osd_dim
    // in third_party/mpv/player/command.c). Returns true when both fields
    // are present and positive.
    //
    // Use this when calling mpv_get_property would be unsafe — specifically
    // the macOS main thread during VO init, where core_thread may be
    // blocked in DispatchQueue.main.sync and a synchronous property read
    // would deadlock (see main.cpp's wait-for-VO loop comment).
    bool read_osd_dims_from_event(mpv_event_property* p, int64_t* w, int64_t* h);
}
