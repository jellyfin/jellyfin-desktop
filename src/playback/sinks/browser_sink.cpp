#include "browser_sink.h"

#include "../../common.h"
#include "../../browser/browsers.h"
#include "../../browser/web_browser.h"
#include "../../browser/overlay_browser.h"
#include "../../browser/about_browser.h"
#include "../../event_dispatcher.h"
#include "../../logging.h"
#include "../../platform/platform.h"

#include "include/cef_parser.h"
#include "include/cef_values.h"

#include <string>

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
    case PlaybackEvent::Kind::MetadataChanged:
    case PlaybackEvent::Kind::ArtworkChanged:
    case PlaybackEvent::Kind::QueueCapsChanged:
    case PlaybackEvent::Kind::Seeked:
        // Not surfaced via this sink. JS already owns metadata.
        break;
    }
}
