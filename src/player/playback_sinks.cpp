#include "playback_sinks.h"

#include "media_session_thread.h"
#include "../common.h"
#include "../browser/browsers.h"
#include "../browser/web_browser.h"
#include "../browser/overlay_browser.h"
#include "../browser/about_browser.h"
#include "../event_dispatcher.h"
#include "../logging.h"
#include "../theme_color.h"
#include "../platform/platform.h"

#include "include/cef_parser.h"
#include "include/cef_values.h"

#include <string>

namespace {
constexpr size_t kEventSinkCapacity = 256;
constexpr size_t kActionSinkCapacity = 64;

void apply_idle_inhibit(const PlaybackSnapshot& snap) {
    if (snap.phase != PlaybackPhase::Playing) {
        g_platform.set_idle_inhibit(IdleInhibitLevel::None);
    } else if (snap.media_type == MediaType::Audio) {
        g_platform.set_idle_inhibit(IdleInhibitLevel::System);
    } else {
        g_platform.set_idle_inhibit(IdleInhibitLevel::Display);
    }
}
}  // namespace

bool CefThreadPlaybackSink::tryPost(const PlaybackEvent& ev) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kEventSinkCapacity) return false;
        queue_.push_back(ev);
    }
    wake_.signal();
    return true;
}

void CefThreadPlaybackSink::pump() {
    std::deque<PlaybackEvent> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(queue_);
    }
    for (const auto& ev : drained) deliver(ev);
}

void BrowserPlaybackSink::deliver(const PlaybackEvent& ev) {
    if (!g_web_browser) return;
    const auto& snap = ev.snapshot;
    switch (ev.kind) {
    case PlaybackEvent::Kind::Started:
        g_web_browser->execJs("window._nativeEmit('playing')");
        break;
    case PlaybackEvent::Kind::Paused:
        g_web_browser->execJs("window._nativeEmit('paused')");
        break;
    case PlaybackEvent::Kind::Finished:
        g_web_browser->execJs("window._nativeEmit('finished')");
        break;
    case PlaybackEvent::Kind::Canceled:
        g_web_browser->execJs("window._nativeEmit('canceled')");
        break;
    case PlaybackEvent::Kind::Error: {
        auto val = CefValue::Create();
        val->SetString(ev.error_message.empty() ? "Playback error"
                                                : ev.error_message);
        auto json = CefWriteJSON(val, JSON_WRITER_DEFAULT);
        g_web_browser->execJs("window._nativeEmit('error',"
                              + json.ToString() + ")");
        break;
    }
    case PlaybackEvent::Kind::SeekingChanged:
        if (ev.flag)
            g_web_browser->execJs("window._nativeEmit('seeking')");
        break;
    case PlaybackEvent::Kind::TrackLoaded:
        // Variant switch (same Jellyfin Id): JS's playerLoad path doesn't
        // fire its own pause UI, so drive the pause indicator from here.
        // Cleared on first-frame Started via the Started → 'playing' emit.
        if (snap.variant_switch_pending)
            g_web_browser->execJs("window._nativeEmit('paused')");
        break;
    case PlaybackEvent::Kind::PositionChanged: {
        int ms = static_cast<int>(snap.position_us / 1000);
        g_web_browser->execJs("window._nativeUpdatePosition("
                              + std::to_string(ms) + ")");
        break;
    }
    case PlaybackEvent::Kind::DurationChanged: {
        int ms = static_cast<int>(snap.duration_us / 1000);
        g_web_browser->execJs("window._nativeUpdateDuration("
                              + std::to_string(ms) + ")");
        break;
    }
    case PlaybackEvent::Kind::RateChanged:
        g_web_browser->execJs("window._nativeSetRate("
                              + std::to_string(snap.rate) + ")");
        break;
    case PlaybackEvent::Kind::FullscreenChanged:
        // Mirror was-maximized so the geometry-save tail in main.cpp can
        // read it after coord shutdown without keeping coord alive.
        g_was_maximized_before_fullscreen = snap.maximized_before_fullscreen;
        g_web_browser->execJs("window._nativeFullscreenChanged("
                              + std::string(snap.fullscreen ? "true" : "false")
                              + ")");
        break;
    case PlaybackEvent::Kind::OsdDimsChanged: {
        if (g_web_browser->browser())
            g_web_browser->resize(snap.layout_w, snap.layout_h, snap.pixel_w, snap.pixel_h);
        if (g_overlay_browser && g_overlay_browser->browser()) {
            g_overlay_browser->resize(snap.layout_w, snap.layout_h, snap.pixel_w, snap.pixel_h);
            g_platform.overlay_resize(snap.layout_w, snap.layout_h, snap.pixel_w, snap.pixel_h);
        }
        if (g_about_browser && g_about_browser->browser()) {
            g_about_browser->resize(snap.layout_w, snap.layout_h, snap.pixel_w, snap.pixel_h);
            g_platform.about_resize(snap.layout_w, snap.layout_h, snap.pixel_w, snap.pixel_h);
        }
        break;
    }
    case PlaybackEvent::Kind::DisplayHzChanged: {
        int hz = snap.display_hz;
        LOG_INFO(LOG_MAIN, "Display refresh rate changed: {} Hz", hz);
        if (g_web_browser->browser())
            g_web_browser->browser()->GetHost()->SetWindowlessFrameRate(hz);
        if (g_overlay_browser && g_overlay_browser->browser())
            g_overlay_browser->browser()->GetHost()->SetWindowlessFrameRate(hz);
        if (g_about_browser && g_about_browser->browser())
            g_about_browser->browser()->GetHost()->SetWindowlessFrameRate(hz);
        break;
    }
    case PlaybackEvent::Kind::BufferedRangesChanged: {
        auto list = CefListValue::Create();
        for (size_t i = 0; i < snap.buffered.size(); ++i) {
            auto range = CefDictionaryValue::Create();
            range->SetDouble("start", static_cast<double>(snap.buffered[i].start_ticks));
            range->SetDouble("end",   static_cast<double>(snap.buffered[i].end_ticks));
            list->SetDictionary(i, range);
        }
        auto val = CefValue::Create();
        val->SetList(list);
        auto json = CefWriteJSON(val, JSON_WRITER_DEFAULT);
        g_web_browser->execJs("window._nativeUpdateBufferedRanges("
                              + json.ToString() + ")");
        break;
    }
    case PlaybackEvent::Kind::BufferingChanged:
    case PlaybackEvent::Kind::MediaTypeChanged:
        // Not surfaced via this sink.
        break;
    }
}

void IdleInhibitSink::deliver(const PlaybackEvent& ev) {
    switch (ev.kind) {
    case PlaybackEvent::Kind::Started:
    case PlaybackEvent::Kind::Paused:
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error:
    case PlaybackEvent::Kind::MediaTypeChanged:
        apply_idle_inhibit(ev.snapshot);
        break;
    default:
        break;
    }
}

void ThemeColorSink::deliver(const PlaybackEvent& ev) {
    if (!g_theme_color) return;
    switch (ev.kind) {
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error:
        g_theme_color->setVideoMode(false);
        break;
    default:
        break;
    }
}

MediaSessionPlaybackSink::MediaSessionPlaybackSink(MediaSessionThread* session)
    : session_(session) {}

bool MediaSessionPlaybackSink::tryPost(const PlaybackEvent& ev) {
    if (!session_) return true;
    const auto& snap = ev.snapshot;
    switch (ev.kind) {
    case PlaybackEvent::Kind::Started:
        session_->setPlaybackState(PlaybackState::Playing);
        // Jump-to position on resume so MPRIS clients see the correct anchor.
        session_->emitSeeked(snap.position_us);
        break;
    case PlaybackEvent::Kind::Paused:
        session_->setPlaybackState(PlaybackState::Paused);
        break;
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error:
        session_->setPlaybackState(PlaybackState::Stopped);
        break;
    case PlaybackEvent::Kind::SeekingChanged:
        session_->setSeeking(ev.flag);
        break;
    case PlaybackEvent::Kind::BufferingChanged:
        session_->setBuffering(ev.flag);
        break;
    case PlaybackEvent::Kind::TrackLoaded:
        // Pre-roll: track is loaded, mpv has not yet flipped pause=false.
        // Map to Paused so macOS/Windows NowPlaying shows the new track
        // immediately, and so MPRIS recompute picks up phase=Starting +
        // the new metadata content_ that the IPC handler already wrote.
        session_->setPlaybackState(PlaybackState::Paused);
        break;
    case PlaybackEvent::Kind::PositionChanged:
        session_->setPosition(snap.position_us);
        break;
    case PlaybackEvent::Kind::RateChanged:
        session_->setRate(snap.rate);
        break;
    case PlaybackEvent::Kind::MediaTypeChanged:
    case PlaybackEvent::Kind::DurationChanged:
    case PlaybackEvent::Kind::FullscreenChanged:
    case PlaybackEvent::Kind::OsdDimsChanged:
    case PlaybackEvent::Kind::BufferedRangesChanged:
    case PlaybackEvent::Kind::DisplayHzChanged:
        // Media metadata IPC carries the type; the rest are not media-session concerns.
        break;
    }
    return true;
}

bool CefThreadActionSink::tryPost(const PlaybackAction& act) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kActionSinkCapacity) return false;
        queue_.push_back(act);
    }
    wake_.signal();
    return true;
}

void CefThreadActionSink::pump() {
    std::deque<PlaybackAction> drained;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drained.swap(queue_);
    }
    for (const auto& a : drained) deliver(a);
}

void MpvActionSink::deliver(const PlaybackAction& act) {
    switch (act.kind) {
    case PlaybackAction::Kind::ApplyPendingTrackSelectionAndPlay:
        g_mpv.ApplyPendingTrackSelectionAndPlay();
        break;
    }
}
