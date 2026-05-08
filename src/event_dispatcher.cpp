#include "event_dispatcher.h"

#include "common.h"
#include "browser/browsers.h"
#include "browser/web_browser.h"
#include "browser/overlay_browser.h"
#include "browser/about_browser.h"
#include "event_queue.h"
#include "logging.h"
#include "player/media_session.h"
#include "player/media_session_thread.h"
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

bool g_was_maximized_before_fullscreen = false;

void update_idle_inhibit() {
    if (g_playback_state.load(std::memory_order_relaxed) != PlaybackState::Playing) {
        g_platform.set_idle_inhibit(IdleInhibitLevel::None);
    } else if (g_media_type.load(std::memory_order_relaxed) == MediaType::Audio) {
        g_platform.set_idle_inhibit(IdleInhibitLevel::System);
    } else {
        g_platform.set_idle_inhibit(IdleInhibitLevel::Display);
    }
}

void publish(const MpvEvent& ev) {
    g_cef_queue.try_push(ev);
}

void cef_consumer_thread() {
#ifdef _WIN32
    HANDLE handles[2] = {
        g_cef_queue.wake_handle(),
        g_shutdown_event.handle()
    };
#else
    int wake_fd = g_cef_queue.wake().fd();
    int shutdown_fd = g_shutdown_event.fd();
    struct pollfd fds[2] = {
        {wake_fd, POLLIN, 0},
        {shutdown_fd, POLLIN, 0},
    };
#endif

    while (true) {
#ifdef _WIN32
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (WaitForSingleObject(handles[1], 0) == WAIT_OBJECT_0) break;
#else
        poll(fds, 2, -1);
        if (fds[1].revents & POLLIN) break;
#endif

        g_cef_queue.drain_wake();
        MpvEvent ev;
        while (g_cef_queue.try_pop(ev)) {
            if (!g_web_browser) continue;
            switch (ev.type) {
            case MpvEventType::PAUSE:
                g_playback_state = ev.flag ? PlaybackState::Paused : PlaybackState::Playing;
                update_idle_inhibit();
                g_web_browser->execJs(ev.flag ? "window._nativeEmit('paused')" : "window._nativeEmit('playing')");
                if (g_media_session)
                    g_media_session->setPlaybackState(ev.flag ? PlaybackState::Paused : PlaybackState::Playing);
                break;
            case MpvEventType::TIME_POS: {
                int ms = static_cast<int>(ev.dbl * 1000);
                g_web_browser->execJs("window._nativeUpdatePosition(" + std::to_string(ms) + ")");
                if (g_media_session)
                    g_media_session->setPosition(static_cast<int64_t>(ev.dbl * 1000000));
                break;
            }
            case MpvEventType::DURATION: {
                int ms = static_cast<int>(ev.dbl * 1000);
                g_web_browser->execJs("window._nativeUpdateDuration(" + std::to_string(ms) + ")");
                break;
            }
            case MpvEventType::FULLSCREEN:
                if (ev.flag) {
                    g_was_maximized_before_fullscreen = mpv::window_maximized();
                } else {
                    g_was_maximized_before_fullscreen = false;
                }
                g_web_browser->execJs("window._nativeFullscreenChanged(" + std::string(ev.flag ? "true" : "false") + ")");
                break;
            case MpvEventType::SPEED:
                g_web_browser->execJs("window._nativeSetRate(" + std::to_string(ev.dbl) + ")");
                if (g_media_session)
                    g_media_session->setRate(ev.dbl);
                break;
            case MpvEventType::SEEKING:
                if (ev.flag) {
                    g_web_browser->execJs("window._nativeEmit('seeking')");
                    if (g_media_session) g_media_session->emitSeeking();
                }
                break;
            case MpvEventType::FILE_LOADED:
                // File loaded paused (see MpvHandle::LoadFile). Apply the
                // pending vid/aid/sid selection and queue the unpause; the
                // PAUSE observer will emit 'playing' to JS once mpv flips
                // pause=false, after the track-switch reinits land. Don't
                // emit 'playing' here — JS must not see "playing" until
                // mpv is actually unpaused with the right tracks selected.
                g_mpv.ApplyPendingTrackSelectionAndPlay();
                break;
            case MpvEventType::END_FILE_EOF:
                g_playback_state = PlaybackState::Stopped;
                update_idle_inhibit();
                g_web_browser->execJs("window._nativeEmit('finished')");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Stopped);
                break;
            case MpvEventType::END_FILE_ERROR: {
                g_playback_state = PlaybackState::Stopped;
                update_idle_inhibit();
                auto val = CefValue::Create();
                val->SetString(ev.err_msg ? ev.err_msg : "Playback error");
                auto json = CefWriteJSON(val, JSON_WRITER_DEFAULT);
                g_web_browser->execJs("window._nativeEmit('error'," + json.ToString() + ")");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Stopped);
                break;
            }
            case MpvEventType::END_FILE_CANCEL:
                g_playback_state = PlaybackState::Stopped;
                update_idle_inhibit();
                g_web_browser->execJs("window._nativeEmit('canceled')");
                if (g_media_session)
                    g_media_session->setPlaybackState(PlaybackState::Stopped);
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
                g_web_browser->execJs("window._nativeUpdateBufferedRanges(" + json.ToString() + ")");
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
