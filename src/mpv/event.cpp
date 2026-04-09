#include "event.h"
#include "handle.h"
#include "../common.h"

void observe_properties(MpvHandle& mpv) {
    mpv.ObservePropertyNode(MPV_OBSERVE_VIDEO_PARAMS, "video-params");
    mpv.ObservePropertyNode(MPV_OBSERVE_OSD_DIMS, "osd-dimensions");
    mpv.ObservePropertyFlag(MPV_OBSERVE_FULLSCREEN, "fullscreen");
    mpv.ObservePropertyFlag(MPV_OBSERVE_PAUSE, "pause");
    mpv.ObservePropertyDouble(MPV_OBSERVE_TIME_POS, "time-pos");
    mpv.ObservePropertyDouble(MPV_OBSERVE_DURATION, "duration");
    mpv.ObservePropertyDouble(MPV_OBSERVE_SPEED, "speed");
    mpv.ObservePropertyFlag(MPV_OBSERVE_SEEKING, "seeking");
    mpv.ObservePropertyDouble(MPV_OBSERVE_DISPLAY_FPS, "display-fps");
}

MpvEvent digest_property(uint64_t id, mpv_event_property* p) {
    MpvEvent ev{};
    switch (id) {
    case MPV_OBSERVE_OSD_DIMS: {
        ev.type = MpvEventType::OSD_DIMS;
        int64_t w = 0, h = 0;
        g_mpv.GetPropertyInt("osd-width", w);
        g_mpv.GetPropertyInt("osd-height", h);
        ev.pw = static_cast<int>(w);
        ev.ph = static_cast<int>(h);
        float scale = g_platform.get_scale();
        ev.lw = static_cast<int>(ev.pw / scale);
        ev.lh = static_cast<int>(ev.ph / scale);
#ifdef __APPLE__
        int qlw = 0, qlh = 0;
        if (g_platform.query_logical_content_size(&qlw, &qlh) && qlw > 0 && qlh > 0) {
            ev.lw = qlw; ev.lh = qlh;
            ev.pw = static_cast<int>(qlw * scale);
            ev.ph = static_cast<int>(qlh * scale);
        }
#endif
        break;
    }
    case MPV_OBSERVE_PAUSE:
        if (p->format != MPV_FORMAT_FLAG) break;
        ev.type = MpvEventType::PAUSE;
        ev.flag = *static_cast<int*>(p->data) != 0;
        break;
    case MPV_OBSERVE_TIME_POS:
        if (p->format != MPV_FORMAT_DOUBLE) break;
        ev.type = MpvEventType::TIME_POS;
        ev.dbl = *static_cast<double*>(p->data);
        break;
    case MPV_OBSERVE_DURATION:
        if (p->format != MPV_FORMAT_DOUBLE) break;
        ev.type = MpvEventType::DURATION;
        ev.dbl = *static_cast<double*>(p->data);
        break;
    case MPV_OBSERVE_FULLSCREEN:
        if (p->format != MPV_FORMAT_FLAG) break;
        ev.type = MpvEventType::FULLSCREEN;
        ev.flag = *static_cast<int*>(p->data) != 0;
        break;
    case MPV_OBSERVE_SPEED:
        if (p->format != MPV_FORMAT_DOUBLE) break;
        ev.type = MpvEventType::SPEED;
        ev.dbl = *static_cast<double*>(p->data);
        break;
    case MPV_OBSERVE_SEEKING:
        if (p->format != MPV_FORMAT_FLAG) break;
        ev.type = MpvEventType::SEEKING;
        ev.flag = *static_cast<int*>(p->data) != 0;
        break;
    case MPV_OBSERVE_DISPLAY_FPS: {
        if (p->format != MPV_FORMAT_DOUBLE) break;
        double fps = *static_cast<double*>(p->data);
        int hz = (fps > 0) ? static_cast<int>(fps + 0.5) : 60;
        if (hz != g_display_hz.load(std::memory_order_relaxed)) {
            g_display_hz.store(hz, std::memory_order_relaxed);
            ev.type = MpvEventType::DISPLAY_FPS;
        }
        break;
    }
    }
    return ev;
}
