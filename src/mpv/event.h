#pragma once

#include <mpv/client.h>
#include <cstdint>

// Observation IDs passed as reply_userdata to mpv_observe_property.
// Mirrors enum `observe_id` in src/playback/src/ingest.rs — both ends
// must agree on which ID maps to which property.
enum MpvObserveId : uint64_t {
    MPV_OBSERVE_OSD_DIMS         = 2,
    MPV_OBSERVE_FULLSCREEN       = 3,
    MPV_OBSERVE_PAUSE            = 4,
    MPV_OBSERVE_TIME_POS         = 5,
    MPV_OBSERVE_DURATION         = 6,
    MPV_OBSERVE_SPEED            = 7,
    MPV_OBSERVE_SEEKING          = 8,
    MPV_OBSERVE_DISPLAY_FPS      = 9,
    MPV_OBSERVE_CACHE_STATE      = 10,
    MPV_OBSERVE_WINDOW_MAX       = 11,
    MPV_OBSERVE_DISPLAY_SCALE    = 12,
    MPV_OBSERVE_PAUSED_FOR_CACHE = 13,
    MPV_OBSERVE_CORE_IDLE        = 14,
    MPV_OBSERVE_VIDEO_FRAME_INFO = 15,
};

class MpvHandle;
enum class DisplayBackend;

// Register the property observations whose IDs are dispatched by the
// Rust ingest layer. Backend selection skips osd-dimensions on Wayland
// (the xdg_toplevel.configure intercept feeds those dims via
// jfn_playback_post_osd_pixels instead).
void observe_properties(MpvHandle& mpv, DisplayBackend backend);

// Thin C++ accessors over the Rust IngestState atomics. Each delegates
// to a jfn_playback_* call in jfn-playback.
namespace mpv {
    bool   fullscreen();
    bool   window_maximized();
    int    osd_pw();
    int    osd_ph();
    int    window_pw();
    int    window_ph();
    double display_scale();
    double display_hz();

    // Push the effective device-pixel size of our window into the
    // geometry-save cache (boot seed + runtime resize).
    void set_window_pixels(int pw, int ph);

    // Wayland authoritative size update: bypasses mpv's osd-dimensions
    // observation. Posts through the same digest path so IngestState
    // atomics and the coordinator OsdDims event both fire.
    void set_osd_dims(int pw, int ph);

    // Sync mpv read for the display refresh rate. Must not be called
    // from an mpv event callback (sync property reads from inside the
    // event thread deadlock).
    void seed_display_hz_sync(MpvHandle& mpv);
}
