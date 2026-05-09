#include "event_dispatcher.h"

#include "common.h"
#include "event_queue.h"
#include "logging.h"
#include "player/playback_coordinator.h"
#include "player/playback_event.h"
#include "player/playback_sinks.h"
#include "wake_event.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#endif

#include <vector>

static EventQueue<MpvEvent> g_cef_queue;
static std::vector<std::shared_ptr<CefThreadPlaybackSink>> g_event_sinks;
static std::vector<std::shared_ptr<CefThreadActionSink>> g_action_sinks;

bool g_was_maximized_before_fullscreen = false;

void register_cef_thread_sinks(
    std::vector<std::shared_ptr<CefThreadPlaybackSink>> event_sinks,
    std::vector<std::shared_ptr<CefThreadActionSink>> action_sinks)
{
    g_event_sinks = std::move(event_sinks);
    g_action_sinks = std::move(action_sinks);
}

void publish(const MpvEvent& ev) {
    g_cef_queue.try_push(ev);
}

namespace {

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
    case MpvEventType::DURATION:
        g_playback_coord->postDuration(static_cast<int64_t>(ev.dbl * 1000000.0));
        break;
    case MpvEventType::SPEED:
        g_playback_coord->postSpeed(ev.dbl);
        break;
    case MpvEventType::FULLSCREEN:
        // Capture the maximized state at the dispatcher boundary so the
        // SM stays free of platform calls. Read mirrors the prior inline
        // capture (only meaningful when entering fullscreen).
        g_playback_coord->postFullscreen(
            ev.flag, ev.flag ? mpv::window_maximized() : false);
        break;
    case MpvEventType::OSD_DIMS:
        g_playback_coord->postOsdDims(ev.lw, ev.lh, ev.pw, ev.ph);
        break;
    case MpvEventType::BUFFERED_RANGES: {
        std::vector<PlaybackBufferedRange> ranges;
        ranges.reserve(static_cast<size_t>(ev.range_count));
        for (int i = 0; i < ev.range_count; i++) {
            ranges.push_back({ev.ranges[i].start_ticks, ev.ranges[i].end_ticks});
        }
        g_playback_coord->postBufferedRanges(std::move(ranges));
        break;
    }
    case MpvEventType::DISPLAY_FPS:
        g_playback_coord->postDisplayHz(g_display_hz.load(std::memory_order_relaxed));
        break;
    default:
        break;
    }
}

}  // namespace

void cef_consumer_thread() {
    // Build poll set: cef_queue + every event sink + every action sink + shutdown.
    const size_t n_event_sinks = g_event_sinks.size();
    const size_t n_action_sinks = g_action_sinks.size();
    const size_t n_fds = 1 + n_event_sinks + n_action_sinks + 1;

#ifdef _WIN32
    std::vector<HANDLE> handles;
    handles.reserve(n_fds);
    handles.push_back(g_cef_queue.wake_handle());
    for (auto& s : g_event_sinks) handles.push_back(s->wake().handle());
    for (auto& s : g_action_sinks) handles.push_back(s->wake().handle());
    handles.push_back(g_shutdown_event.handle());
    const size_t shutdown_idx = n_fds - 1;
#else
    std::vector<struct pollfd> fds;
    fds.reserve(n_fds);
    fds.push_back({g_cef_queue.wake().fd(), POLLIN, 0});
    for (auto& s : g_event_sinks) fds.push_back({s->wake().fd(), POLLIN, 0});
    for (auto& s : g_action_sinks) fds.push_back({s->wake().fd(), POLLIN, 0});
    fds.push_back({g_shutdown_event.fd(), POLLIN, 0});
    const size_t shutdown_idx = n_fds - 1;
#endif

    while (true) {
#ifdef _WIN32
        WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
                               handles.data(), FALSE, INFINITE);
        if (WaitForSingleObject(handles[shutdown_idx], 0) == WAIT_OBJECT_0) break;
#else
        poll(fds.data(), fds.size(), -1);
        if (fds[shutdown_idx].revents & POLLIN) break;
#endif

        // Drain coord-emitted events first so any same-batch raw mpv events
        // observe the post-transition snapshot via subsequent route_to_coordinator
        // calls. (Sinks no longer pull from coord; this ordering is now only a
        // freshness preference, not a correctness requirement.)
        for (auto& s : g_event_sinks) {
            s->wake().drain();
            s->pump();
        }
        for (auto& s : g_action_sinks) {
            s->wake().drain();
            s->pump();
        }

        g_cef_queue.drain_wake();
        MpvEvent ev;
        while (g_cef_queue.try_pop(ev)) {
            if (ev.type == MpvEventType::SHUTDOWN) return;
            route_to_coordinator(ev);
        }
    }
}
