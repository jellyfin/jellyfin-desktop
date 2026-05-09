#include "event_dispatcher.h"

#include "common.h"
#include "browser/browsers.h"
#include "browser/web_browser.h"
#include "browser/overlay_browser.h"
#include "browser/about_browser.h"
#include "event_queue.h"
#include "logging.h"
#include "player/media_session_thread.h"
#include "player/playback_coordinator.h"
#include "player/playback_event.h"
#include "player/playback_sinks.h"
#include "theme_color.h"
#include "wake_event.h"

#include "include/cef_parser.h"
#include "include/cef_values.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#endif

#include <string>

static EventQueue<MpvEvent> g_cef_queue;
static std::shared_ptr<BrowserPlaybackSink> g_browser_sink;

bool g_was_maximized_before_fullscreen = false;

void set_browser_playback_sink(std::shared_ptr<BrowserPlaybackSink> sink) {
    g_browser_sink = std::move(sink);
}

void publish(const MpvEvent& ev) {
    g_cef_queue.try_push(ev);
}

namespace {

void apply_idle_inhibit(const PlaybackSnapshot& snap) {
    if (snap.phase != PlaybackPhase::Playing) {
        g_platform.set_idle_inhibit(IdleInhibitLevel::None);
    } else if (snap.media_type == MediaType::Audio) {
        g_platform.set_idle_inhibit(IdleInhibitLevel::System);
    } else {
        g_platform.set_idle_inhibit(IdleInhibitLevel::Display);
    }
}

void deliver_playback_event(const PlaybackEvent& ev) {
    PlaybackSnapshot snap = g_playback_coord ? g_playback_coord->snapshot()
                                             : PlaybackSnapshot{};
    {
        const char* k = "?";
        switch (ev.kind) {
        case PlaybackEvent::Kind::Started:           k = "Started"; break;
        case PlaybackEvent::Kind::Paused:            k = "Paused"; break;
        case PlaybackEvent::Kind::Finished:          k = "Finished"; break;
        case PlaybackEvent::Kind::Canceled:          k = "Canceled"; break;
        case PlaybackEvent::Kind::Error:             k = "Error"; break;
        case PlaybackEvent::Kind::SeekingChanged:    k = "SeekingChanged"; break;
        case PlaybackEvent::Kind::BufferingChanged:  k = "BufferingChanged"; break;
        case PlaybackEvent::Kind::MediaTypeChanged:  k = "MediaTypeChanged"; break;
        case PlaybackEvent::Kind::TrackLoaded:       k = "TrackLoaded"; break;
        }
        LOG_INFO(LOG_MEDIA,
            "coord emit: {} flag={} snap[phase={} buffer={} seek={}]",
            k, ev.flag, static_cast<int>(snap.phase),
            snap.buffering, snap.seeking);
    }
    if (!g_web_browser) return;
    switch (ev.kind) {
    case PlaybackEvent::Kind::Started:
        g_web_browser->execJs("window._nativeEmit('playing')");
        apply_idle_inhibit(snap);
        if (g_media_session)
            g_media_session->emitSeeked(snap.position_us);
        break;
    case PlaybackEvent::Kind::Paused:
        g_web_browser->execJs("window._nativeEmit('paused')");
        apply_idle_inhibit(snap);
        break;
    case PlaybackEvent::Kind::Finished:
        g_web_browser->execJs("window._nativeEmit('finished')");
        if (g_theme_color) g_theme_color->setVideoMode(false);
        apply_idle_inhibit(snap);
        break;
    case PlaybackEvent::Kind::Canceled:
        g_web_browser->execJs("window._nativeEmit('canceled')");
        if (g_theme_color) g_theme_color->setVideoMode(false);
        apply_idle_inhibit(snap);
        break;
    case PlaybackEvent::Kind::Error: {
        auto val = CefValue::Create();
        val->SetString(ev.error_message.empty() ? "Playback error"
                                                : ev.error_message);
        auto json = CefWriteJSON(val, JSON_WRITER_DEFAULT);
        g_web_browser->execJs("window._nativeEmit('error',"
                              + json.ToString() + ")");
        if (g_theme_color) g_theme_color->setVideoMode(false);
        apply_idle_inhibit(snap);
        break;
    }
    case PlaybackEvent::Kind::SeekingChanged:
        if (ev.flag)
            g_web_browser->execJs("window._nativeEmit('seeking')");
        break;
    case PlaybackEvent::Kind::BufferingChanged:
        // Buffering is reflected via the snapshot; no JS emit today.
        break;
    case PlaybackEvent::Kind::MediaTypeChanged:
        apply_idle_inhibit(snap);
        break;
    case PlaybackEvent::Kind::TrackLoaded:
        // JS already drives its own per-track state via playerLoad on
        // a fresh load. The exception is a variant switch (same
        // Jellyfin Id) — JS's playerLoad path doesn't fire its own
        // pause UI, so the SM's variant_switch_pending lifecycle drives
        // the pause indicator from here. Cleared on first-frame Started
        // via the existing Started → 'playing' emit.
        if (snap.variant_switch_pending)
            g_web_browser->execJs("window._nativeEmit('paused')");
        // Idle inhibit unchanged because phase=Starting still maps to
        // "not Playing" for that gate.
        break;
    }
}

void route_to_coordinator(const MpvEvent& ev) {
    if (!g_playback_coord) return;
    switch (ev.type) {
    case MpvEventType::PAUSE:
        LOG_INFO(LOG_MPV, "mpv: pause={}", ev.flag);
        g_playback_coord->postPauseChanged(ev.flag);
        break;
    case MpvEventType::FILE_LOADED:
        LOG_INFO(LOG_MPV, "mpv: FILE_LOADED");
        g_playback_coord->postFileLoaded();
        break;
    case MpvEventType::END_FILE_EOF:
        LOG_INFO(LOG_MPV, "mpv: END_FILE eof");
        g_playback_coord->postEndFile(EndReason::Eof);
        break;
    case MpvEventType::END_FILE_ERROR:
        LOG_INFO(LOG_MPV, "mpv: END_FILE error msg={}", ev.err_msg ? ev.err_msg : "");
        g_playback_coord->postEndFile(EndReason::Error,
                                      ev.err_msg ? ev.err_msg : "");
        break;
    case MpvEventType::END_FILE_CANCEL:
        LOG_INFO(LOG_MPV, "mpv: END_FILE cancel");
        g_playback_coord->postEndFile(EndReason::Canceled);
        break;
    case MpvEventType::SEEKING:
        LOG_INFO(LOG_MPV, "mpv: seeking={}", ev.flag);
        g_playback_coord->postSeekingChanged(ev.flag);
        break;
    case MpvEventType::PAUSED_FOR_CACHE:
        LOG_INFO(LOG_MPV, "mpv: paused-for-cache={}", ev.flag);
        g_playback_coord->postPausedForCache(ev.flag);
        break;
    case MpvEventType::CORE_IDLE:
        LOG_INFO(LOG_MPV, "mpv: core-idle={}", ev.flag);
        g_playback_coord->postCoreIdle(ev.flag);
        break;
    case MpvEventType::TIME_POS:
        g_playback_coord->postPosition(static_cast<int64_t>(ev.dbl * 1000000.0));
        break;
    case MpvEventType::VIDEO_FRAME_INFO:
        g_playback_coord->postVideoFrameAvailable(ev.flag);
        break;
    default:
        break;
    }
}

}  // namespace

void cef_consumer_thread() {
#ifdef _WIN32
    HANDLE handles[3] = {
        g_cef_queue.wake_handle(),
        g_browser_sink->wake().handle(),
        g_shutdown_event.handle()
    };
#else
    struct pollfd fds[3] = {
        {g_cef_queue.wake().fd(),         POLLIN, 0},
        {g_browser_sink->wake().fd(),     POLLIN, 0},
        {g_shutdown_event.fd(),           POLLIN, 0},
    };
#endif

    while (true) {
#ifdef _WIN32
        WaitForMultipleObjects(3, handles, FALSE, INFINITE);
        if (WaitForSingleObject(handles[2], 0) == WAIT_OBJECT_0) break;
#else
        poll(fds, 3, -1);
        if (fds[2].revents & POLLIN) break;
#endif

        // Drain coordinator-emitted playback events first so snapshot-driven
        // side effects in mpv-event handling see the updated state.
        g_browser_sink->wake().drain();
        PlaybackEvent pe;
        while (g_browser_sink->drain(pe)) {
            deliver_playback_event(pe);
        }

        g_cef_queue.drain_wake();
        MpvEvent ev;
        while (g_cef_queue.try_pop(ev)) {
            // Playback-relevant events flow through the coordinator;
            // it owns transition logic and side-effect dispatch.
            route_to_coordinator(ev);

            if (!g_web_browser) {
                if (ev.type == MpvEventType::SHUTDOWN) return;
                continue;
            }
            switch (ev.type) {
            case MpvEventType::PAUSE:
            case MpvEventType::FILE_LOADED:
            case MpvEventType::END_FILE_EOF:
            case MpvEventType::END_FILE_ERROR:
            case MpvEventType::END_FILE_CANCEL:
            case MpvEventType::SEEKING:
            case MpvEventType::PAUSED_FOR_CACHE:
                // Coordinator-owned. UI fan-out happens via BrowserPlaybackSink.
                if (ev.type == MpvEventType::FILE_LOADED) {
                    // mpv loads paused (see MpvHandle::LoadFile). Apply pending
                    // vid/aid/sid selection and queue the unpause; the PAUSE
                    // observer will drive Started once mpv flips pause=false
                    // after the track-switch reinits.
                    g_mpv.ApplyPendingTrackSelectionAndPlay();
                }
                break;
            case MpvEventType::TIME_POS: {
                int ms = static_cast<int>(ev.dbl * 1000);
                g_web_browser->execJs("window._nativeUpdatePosition("
                                      + std::to_string(ms) + ")");
                if (g_media_session)
                    g_media_session->setPosition(
                        static_cast<int64_t>(ev.dbl * 1000000));
                break;
            }
            case MpvEventType::DURATION: {
                int ms = static_cast<int>(ev.dbl * 1000);
                g_web_browser->execJs("window._nativeUpdateDuration("
                                      + std::to_string(ms) + ")");
                break;
            }
            case MpvEventType::FULLSCREEN:
                if (ev.flag) {
                    g_was_maximized_before_fullscreen = mpv::window_maximized();
                } else {
                    g_was_maximized_before_fullscreen = false;
                }
                g_web_browser->execJs("window._nativeFullscreenChanged("
                                      + std::string(ev.flag ? "true" : "false")
                                      + ")");
                break;
            case MpvEventType::SPEED:
                g_web_browser->execJs("window._nativeSetRate("
                                      + std::to_string(ev.dbl) + ")");
                if (g_media_session) g_media_session->setRate(ev.dbl);
                break;
            case MpvEventType::OSD_DIMS:
                if (g_web_browser->browser())
                    g_web_browser->resize(ev.lw, ev.lh, ev.pw, ev.ph);
                if (g_overlay_browser && g_overlay_browser->browser()) {
                    g_overlay_browser->resize(ev.lw, ev.lh, ev.pw, ev.ph);
                    g_platform.overlay_resize(ev.lw, ev.lh, ev.pw, ev.ph);
                }
                if (g_about_browser && g_about_browser->browser()) {
                    g_about_browser->resize(ev.lw, ev.lh, ev.pw, ev.ph);
                    g_platform.about_resize(ev.lw, ev.lh, ev.pw, ev.ph);
                }
                break;
            case MpvEventType::BUFFERED_RANGES: {
                auto list = CefListValue::Create();
                for (int i = 0; i < ev.range_count; i++) {
                    auto range = CefDictionaryValue::Create();
                    range->SetDouble("start", static_cast<double>(ev.ranges[i].start_ticks));
                    range->SetDouble("end", static_cast<double>(ev.ranges[i].end_ticks));
                    list->SetDictionary(static_cast<size_t>(i), range);
                }
                auto val = CefValue::Create();
                val->SetList(list);
                auto json = CefWriteJSON(val, JSON_WRITER_DEFAULT);
                g_web_browser->execJs("window._nativeUpdateBufferedRanges("
                                      + json.ToString() + ")");
                break;
            }
            case MpvEventType::DISPLAY_FPS: {
                int hz = g_display_hz.load(std::memory_order_relaxed);
                LOG_INFO(LOG_MAIN, "Display refresh rate changed: {} Hz", hz);
                if (g_web_browser && g_web_browser->browser())
                    g_web_browser->browser()->GetHost()->SetWindowlessFrameRate(hz);
                if (g_overlay_browser && g_overlay_browser->browser())
                    g_overlay_browser->browser()->GetHost()->SetWindowlessFrameRate(hz);
                if (g_about_browser && g_about_browser->browser())
                    g_about_browser->browser()->GetHost()->SetWindowlessFrameRate(hz);
                break;
            }
            case MpvEventType::SHUTDOWN:
                return;
            default:
                break;
            }
        }
    }
}
