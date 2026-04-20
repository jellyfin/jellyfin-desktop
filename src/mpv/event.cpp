#include "event.h"
#include "handle.h"
#include "../common.h"
#ifdef __APPLE__
#include "../platform/macos_platform.h"
#endif
#include <atomic>
#include <cstring>

static std::atomic<bool>   s_fullscreen{false};
static std::atomic<bool>   s_window_maximized{false};
static std::atomic<int>    s_osd_pw{0};
static std::atomic<int>    s_osd_ph{0};
static std::atomic<int>    s_window_pw{0};
static std::atomic<int>    s_window_ph{0};
static std::atomic<double> s_display_scale{0.0};

namespace mpv {
    bool   fullscreen()       { return s_fullscreen.load(std::memory_order_relaxed); }
    bool   window_maximized() { return s_window_maximized.load(std::memory_order_relaxed); }
    int    osd_pw()           { return s_osd_pw.load(std::memory_order_relaxed); }
    int    osd_ph()           { return s_osd_ph.load(std::memory_order_relaxed); }
    int    window_pw()        { return s_window_pw.load(std::memory_order_relaxed); }
    int    window_ph()        { return s_window_ph.load(std::memory_order_relaxed); }
    double display_scale()    { return s_display_scale.load(std::memory_order_relaxed); }

    void set_window_pixels(int pw, int ph) {
        s_window_pw.store(pw, std::memory_order_relaxed);
        s_window_ph.store(ph, std::memory_order_relaxed);
    }

    bool read_osd_dims_from_event(mpv_event_property* p, int64_t* w, int64_t* h) {
        if (!p || p->format != MPV_FORMAT_NODE || !p->data) return false;
        auto* n = static_cast<mpv_node*>(p->data);
        if (n->format != MPV_FORMAT_NODE_MAP || !n->u.list) return false;
        int64_t lw = 0, lh = 0;
        for (int i = 0; i < n->u.list->num; i++) {
            const mpv_node& v = n->u.list->values[i];
            if (v.format != MPV_FORMAT_INT64) continue;
            const char* k = n->u.list->keys[i];
            if (!strcmp(k, "w")) lw = v.u.int64;
            else if (!strcmp(k, "h")) lh = v.u.int64;
        }
        if (lw <= 0 || lh <= 0) return false;
        *w = lw; *h = lh;
        return true;
    }
}

void observe_properties(MpvHandle& mpv) {
    // Register display-hidpi-scale before osd-dimensions so mpv's initial
    // value delivery (FIFO by observation time) seeds s_display_scale
    // before the first osd-dimensions event, which consumes the scale to
    // compute logical dims.
    mpv.ObservePropertyDouble(MPV_OBSERVE_DISPLAY_SCALE, "display-hidpi-scale");
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
}

MpvEvent digest_property(uint64_t id, mpv_event_property* p) {
    MpvEvent ev{};
    switch (id) {
    case MPV_OBSERVE_OSD_DIMS: {
        ev.type = MpvEventType::OSD_DIMS;
        int64_t w = 0, h = 0;
        if (!mpv::read_osd_dims_from_event(p, &w, &h)) {
            ev.type = MpvEventType::NONE;
            break;
        }
        ev.pw = static_cast<int>(w);
        ev.ph = static_cast<int>(h);
        s_osd_pw.store(ev.pw, std::memory_order_relaxed);
        s_osd_ph.store(ev.ph, std::memory_order_relaxed);
        float scale = g_platform.get_scale();
        ev.lw = static_cast<int>(ev.pw / scale);
        ev.lh = static_cast<int>(ev.ph / scale);
#ifdef __APPLE__
        int qlw = 0, qlh = 0;
        if (macos_platform::query_logical_content_size(&qlw, &qlh) && qlw > 0 && qlh > 0) {
            ev.lw = qlw; ev.lh = qlh;
            ev.pw = static_cast<int>(qlw * scale);
            ev.ph = static_cast<int>(qlh * scale);
        }
#endif
        // Keep the "effective pixel size" cache current so shutdown's
        // geometry save reflects the latest resize, not just the boot-time
        // value seeded by set_window_pixels.
        mpv::set_window_pixels(ev.pw, ev.ph);
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
        s_fullscreen.store(ev.flag, std::memory_order_relaxed);
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
    case MPV_OBSERVE_WINDOW_MAX:
        // Silent update: callers read mpv::window_maximized() on demand.
        if (p->format != MPV_FORMAT_FLAG) break;
        s_window_maximized.store(*static_cast<int*>(p->data) != 0,
                                 std::memory_order_relaxed);
        break;
    case MPV_OBSERVE_DISPLAY_SCALE:
        // Silent update: callers read mpv::display_scale() on demand.
        if (p->format != MPV_FORMAT_DOUBLE) break;
        s_display_scale.store(*static_cast<double*>(p->data),
                              std::memory_order_relaxed);
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
    case MPV_OBSERVE_CACHE_STATE: {
        if (p->format != MPV_FORMAT_NODE) break;
        auto* node = static_cast<mpv_node*>(p->data);
        if (!node || node->format != MPV_FORMAT_NODE_MAP) break;
        ev.type = MpvEventType::BUFFERED_RANGES;
        ev.range_count = 0;
        for (int i = 0; i < node->u.list->num; i++) {
            if (strcmp(node->u.list->keys[i], "seekable-ranges") != 0) continue;
            mpv_node* arr = &node->u.list->values[i];
            if (arr->format != MPV_FORMAT_NODE_ARRAY) break;
            for (int j = 0; j < arr->u.list->num && ev.range_count < MAX_BUFFERED_RANGES; j++) {
                mpv_node* range = &arr->u.list->values[j];
                if (range->format != MPV_FORMAT_NODE_MAP) continue;
                double start = 0, end = 0;
                for (int k = 0; k < range->u.list->num; k++) {
                    if (strcmp(range->u.list->keys[k], "start") == 0 &&
                        range->u.list->values[k].format == MPV_FORMAT_DOUBLE)
                        start = range->u.list->values[k].u.double_;
                    else if (strcmp(range->u.list->keys[k], "end") == 0 &&
                             range->u.list->values[k].format == MPV_FORMAT_DOUBLE)
                        end = range->u.list->values[k].u.double_;
                }
                ev.ranges[ev.range_count++] = {
                    static_cast<int64_t>(start * 10000000.0),
                    static_cast<int64_t>(end * 10000000.0)
                };
            }
            break;
        }
        break;
    }
    }
    return ev;
}
