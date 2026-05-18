#include "event.h"
#include "handle.h"
#include "../common.h"
#include "../platform/display_backend.h"
#include "../playback/jfn_ingest.h"

namespace mpv {
    bool   fullscreen()       { return jfn_playback_fullscreen(); }
    bool   window_maximized() { return jfn_playback_window_maximized(); }
    int    osd_pw()           { return jfn_playback_osd_pw(); }
    int    osd_ph()           { return jfn_playback_osd_ph(); }
    int    window_pw()        { return jfn_playback_window_pw(); }
    int    window_ph()        { return jfn_playback_window_ph(); }
    double display_scale()    { return jfn_playback_display_scale(); }
    double display_hz()       { return jfn_playback_display_hz(); }

    void set_window_pixels(int pw, int ph) {
        jfn_playback_set_window_pixels(pw, ph);
    }

    void seed_display_hz_sync(MpvHandle& mpv) {
        double fps = 0.0;
        mpv_get_property(mpv.Get(), "display-fps", MPV_FORMAT_DOUBLE, &fps);
        if (fps > 0) jfn_playback_set_display_hz(fps);
    }

    void set_osd_dims(int pw, int ph) {
        if (pw <= 0 || ph <= 0) return;
        float scale = g_platform.get_scale ? g_platform.get_scale() : 1.0f;
        if (scale <= 0.f) scale = 1.0f;
        jfn_playback_post_osd_pixels(pw, ph, scale, false, 0, 0);
    }
}

void observe_properties(MpvHandle& mpv, DisplayBackend backend) {
    // Register display-hidpi-scale before osd-dimensions so mpv's initial
    // value delivery (FIFO by observation time) seeds the display-scale
    // cache before the first osd-dimensions event, which consumes the
    // scale to compute logical dims.
    mpv.ObservePropertyDouble(MPV_OBSERVE_DISPLAY_SCALE, "display-hidpi-scale");
    // On Wayland the proxy's xdg_toplevel.configure intercept drives the
    // OSD_DIMS path (see platform::wayland::on_proxy_configure +
    // mpv::set_osd_dims). Observing mpv's osd-dimensions there would
    // double-post identical values to the coordinator.
    if (backend != DisplayBackend::Wayland)
        mpv.ObservePropertyNode(MPV_OBSERVE_OSD_DIMS, "osd-dimensions");
    mpv.ObservePropertyFlag(MPV_OBSERVE_FULLSCREEN, "fullscreen");
    mpv.ObservePropertyFlag(MPV_OBSERVE_PAUSE, "pause");
    mpv.ObservePropertyDouble(MPV_OBSERVE_TIME_POS, "time-pos");
    mpv.ObservePropertyDouble(MPV_OBSERVE_DURATION, "duration");
    mpv.ObservePropertyDouble(MPV_OBSERVE_SPEED, "speed");
    mpv.ObservePropertyFlag(MPV_OBSERVE_SEEKING, "seeking");
    mpv.ObservePropertyDouble(MPV_OBSERVE_DISPLAY_FPS, "display-fps");
    mpv.ObservePropertyNode(MPV_OBSERVE_CACHE_STATE, "demuxer-cache-state");
    mpv.ObservePropertyFlag(MPV_OBSERVE_WINDOW_MAX, "window-maximized");
    mpv.ObservePropertyFlag(MPV_OBSERVE_PAUSED_FOR_CACHE, "paused-for-cache");
    mpv.ObservePropertyFlag(MPV_OBSERVE_CORE_IDLE, "core-idle");
    mpv.ObservePropertyNode(MPV_OBSERVE_VIDEO_FRAME_INFO, "video-frame-info");
}
