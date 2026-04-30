#include "player_router.h"
#include "track_resolver.h"

#include "../cef/message_bus.h"
#include "../common.h"
#include "logging.h"

#include "include/cef_values.h"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace player_router {

namespace {

// =====================================================================
// State
// =====================================================================

// Streams cached on player.load and consulted on selectAudio/selectSubtitle.
// Owned and mutated solely on TID_UI.
std::vector<track_resolver::MediaStream> g_streams;

// Last-known time-pos in ms, sourced from MPV_OBSERVE_TIME_POS. Used to
// reply synchronously to player.getPosition.
int64_t g_last_position_ms = 0;

// =====================================================================
// Helpers
// =====================================================================

std::optional<int> opt_int(const CefRefPtr<CefDictionaryValue>& d,
                           const std::string& key) {
    if (!d->HasKey(key)) return std::nullopt;
    auto t = d->GetType(key);
    if (t == VTYPE_NULL) return std::nullopt;
    if (t == VTYPE_INT) return d->GetInt(key);
    if (t == VTYPE_DOUBLE) return static_cast<int>(std::lround(d->GetDouble(key)));
    return std::nullopt;
}

double get_double(const CefRefPtr<CefDictionaryValue>& d,
                  const std::string& key, double fallback = 0.0) {
    if (!d->HasKey(key)) return fallback;
    auto t = d->GetType(key);
    if (t == VTYPE_DOUBLE) return d->GetDouble(key);
    if (t == VTYPE_INT)    return static_cast<double>(d->GetInt(key));
    return fallback;
}

int64_t get_int64(const CefRefPtr<CefDictionaryValue>& d,
                  const std::string& key, int64_t fallback = 0) {
    if (!d->HasKey(key)) return fallback;
    auto t = d->GetType(key);
    if (t == VTYPE_INT)    return d->GetInt(key);
    if (t == VTYPE_DOUBLE) return static_cast<int64_t>(std::llround(d->GetDouble(key)));
    return fallback;
}

// Parse Jellyfin MediaSource.MediaStreams from a CefListValue into the
// resolver's pure-C++ shape. Lives here, not in track_resolver, because
// track_resolver is kept CEF-free for doctest unit testing.
std::vector<track_resolver::MediaStream>
parse_media_streams(CefRefPtr<CefListValue> list) {
    std::vector<track_resolver::MediaStream> out;
    if (!list) return out;
    out.reserve(list->GetSize());
    for (size_t i = 0; i < list->GetSize(); i++) {
        if (list->GetType(i) != VTYPE_DICTIONARY) continue;
        auto d = list->GetDictionary(i);
        track_resolver::MediaStream s;
        if (d->HasKey("Index"))
            s.index = d->GetInt("Index");
        if (d->HasKey("Type") && d->GetType("Type") == VTYPE_STRING)
            s.type = d->GetString("Type").ToString();
        if (d->HasKey("IsExternal") && d->GetType("IsExternal") == VTYPE_BOOL)
            s.is_external = d->GetBool("IsExternal");
        if (d->HasKey("DeliveryMethod") && d->GetType("DeliveryMethod") == VTYPE_STRING)
            s.delivery_method = d->GetString("DeliveryMethod").ToString();
        if (d->HasKey("DeliveryUrl") && d->GetType("DeliveryUrl") == VTYPE_STRING)
            s.delivery_url = d->GetString("DeliveryUrl").ToString();
        out.push_back(std::move(s));
    }
    return out;
}

CefRefPtr<CefDictionaryValue> empty_payload() {
    return CefDictionaryValue::Create();
}

void emit(const std::string& name, CefRefPtr<CefDictionaryValue> payload) {
    g_bus.emit(name, payload ? payload : empty_payload());
}

// =====================================================================
// Inbound handlers
// =====================================================================

void on_load(CefRefPtr<CefDictionaryValue> p) {
    if (!g_mpv.IsValid()) return;
    std::string url = p->HasKey("url") ? p->GetString("url").ToString() : "";
    int64_t startMs = get_int64(p, "startMs", 0);
    auto defaultAudio = opt_int(p, "defaultAudioIdx");
    auto defaultSub   = opt_int(p, "defaultSubIdx");

    g_streams.clear();
    if (p->HasKey("mediaSource") && p->GetType("mediaSource") == VTYPE_DICTIONARY) {
        auto ms = p->GetDictionary("mediaSource");
        if (ms->HasKey("MediaStreams") && ms->GetType("MediaStreams") == VTYPE_LIST)
            g_streams = parse_media_streams(ms->GetList("MediaStreams"));
    }

    int64_t aid = track_resolver::resolve_audio_aid(g_streams, defaultAudio);
    auto sub = track_resolver::resolve_subtitle(
        g_streams, defaultSub,
        track_resolver::SubtitleUnresolvedFallback::AutoFallback);

    MpvHandle::LoadOptions opts;
    opts.startSecs = startMs / 1000.0;
    opts.audioTrack = aid;
    if (sub.kind == track_resolver::SubtitleSelection::Kind::Embedded)
        opts.subTrack = sub.sid;
    else
        opts.subTrack = MpvHandle::kTrackDisable;

    LOG_INFO(LOG_CEF, "player.load: audio={} sub={} start={}ms url={}",
             aid, opts.subTrack, startMs, url);
    g_mpv.LoadFile(url, opts);

    if (sub.kind == track_resolver::SubtitleSelection::Kind::External)
        g_mpv.SubAdd(sub.url);
}

void on_play (CefRefPtr<CefDictionaryValue>) { if (g_mpv.IsValid()) g_mpv.Play(); }
void on_pause(CefRefPtr<CefDictionaryValue>) { if (g_mpv.IsValid()) g_mpv.Pause(); }
void on_stop (CefRefPtr<CefDictionaryValue>) { if (g_mpv.IsValid()) g_mpv.Stop(); }

void on_seek(CefRefPtr<CefDictionaryValue> p) {
    if (g_mpv.IsValid())
        g_mpv.SeekAbsolute(get_int64(p, "positionMs", 0) / 1000.0);
}

void on_select_audio(CefRefPtr<CefDictionaryValue> p) {
    if (!g_mpv.IsValid()) return;
    auto idx = opt_int(p, "jellyfinIndex");
    int64_t aid = track_resolver::resolve_audio_aid(g_streams, idx);
    LOG_INFO(LOG_CEF, "player.selectAudio: jf={} aid={}",
             idx ? std::to_string(*idx) : "null", aid);
    g_mpv.SetAudioTrack(aid);
}

void on_select_subtitle(CefRefPtr<CefDictionaryValue> p) {
    if (!g_mpv.IsValid()) return;
    auto idx = opt_int(p, "jellyfinIndex");
    auto sub = track_resolver::resolve_subtitle(
        g_streams, idx,
        track_resolver::SubtitleUnresolvedFallback::DisableFallback);
    switch (sub.kind) {
    case track_resolver::SubtitleSelection::Kind::Disable:
        LOG_INFO(LOG_CEF, "player.selectSubtitle: disable");
        g_mpv.SetSubtitleTrack(track_resolver::kTrackDisable);
        break;
    case track_resolver::SubtitleSelection::Kind::Embedded:
        LOG_INFO(LOG_CEF, "player.selectSubtitle: sid={}", sub.sid);
        g_mpv.SetSubtitleTrack(sub.sid);
        break;
    case track_resolver::SubtitleSelection::Kind::External:
        LOG_INFO(LOG_CEF, "player.selectSubtitle: external {}", sub.url);
        g_mpv.SubAdd(sub.url);
        break;
    }
}

void on_set_rate(CefRefPtr<CefDictionaryValue> p) {
    if (g_mpv.IsValid()) g_mpv.SetSpeed(get_double(p, "rate", 1.0));
}

void on_set_subtitle_offset(CefRefPtr<CefDictionaryValue> p) {
    if (g_mpv.IsValid()) g_mpv.SetAudioDelay(-get_double(p, "seconds", 0.0));
    // NOTE: sub-delay would be more correct than audio-delay, but the
    // existing code path used audio-delay (negative for sub-shift). Preserve
    // legacy behavior in this scaffolding step; the JS migration step
    // revisits semantics.
}

void on_set_volume(CefRefPtr<CefDictionaryValue> p) {
    if (g_mpv.IsValid()) g_mpv.SetVolume(get_double(p, "volume", 100.0));
}

void on_set_muted(CefRefPtr<CefDictionaryValue> p) {
    if (g_mpv.IsValid() && p->HasKey("muted"))
        g_mpv.SetMuted(p->GetBool("muted"));
}

void on_set_aspect_ratio(CefRefPtr<CefDictionaryValue> p) {
    if (!g_mpv.IsValid()) return;
    if (p->HasKey("mode") && p->GetType("mode") == VTYPE_STRING)
        g_mpv.SetAspectMode(p->GetString("mode").ToString());
}

void on_set_video_rectangle(CefRefPtr<CefDictionaryValue>) {
    // Currently a no-op: the desktop client renders mpv on the parent surface
    // and CEF as a subsurface; geometry is owned by the platform layer, not
    // jellyfin-web. Kept registered so messages don't fall through to the
    // "no handler" warning path.
}

void on_get_position(CefRefPtr<CefDictionaryValue> p) {
    auto reply = CefDictionaryValue::Create();
    if (p->HasKey("requestId"))
        reply->SetValue("requestId", p->GetValue("requestId")->Copy());
    reply->SetInt("positionMs", static_cast<int>(g_last_position_ms));
    emit("player.positionReply", reply);
}

}  // namespace

// =====================================================================
// Public entry points
// =====================================================================

void install() {
    g_bus.on("player.load",              on_load);
    g_bus.on("player.play",              on_play);
    g_bus.on("player.pause",             on_pause);
    g_bus.on("player.stop",              on_stop);
    g_bus.on("player.seek",              on_seek);
    g_bus.on("player.selectAudio",       on_select_audio);
    g_bus.on("player.selectSubtitle",    on_select_subtitle);
    g_bus.on("player.setRate",           on_set_rate);
    g_bus.on("player.setSubtitleOffset", on_set_subtitle_offset);
    g_bus.on("player.setVolume",         on_set_volume);
    g_bus.on("player.setMuted",          on_set_muted);
    g_bus.on("player.setAspectRatio",    on_set_aspect_ratio);
    g_bus.on("player.setVideoRectangle", on_set_video_rectangle);
    g_bus.on("player.getPosition",       on_get_position);
}

void on_mpv_event(const MpvEvent& ev) {
    switch (ev.type) {
    case MpvEventType::PAUSE:
        emit(ev.flag ? "player.paused" : "player.playing", nullptr);
        break;
    case MpvEventType::TIME_POS: {
        g_last_position_ms = static_cast<int64_t>(ev.dbl * 1000);
        auto p = CefDictionaryValue::Create();
        p->SetInt("positionMs", static_cast<int>(g_last_position_ms));
        emit("player.tick", p);
        break;
    }
    case MpvEventType::DURATION: {
        auto p = CefDictionaryValue::Create();
        p->SetInt("durationMs", static_cast<int>(ev.dbl * 1000));
        emit("player.durationChanged", p);
        break;
    }
    case MpvEventType::SPEED: {
        auto p = CefDictionaryValue::Create();
        p->SetDouble("rate", ev.dbl);
        emit("player.rateChanged", p);
        break;
    }
    case MpvEventType::SEEKING: {
        auto p = CefDictionaryValue::Create();
        p->SetBool("active", ev.flag);
        emit("player.seeking", p);
        break;
    }
    case MpvEventType::FILE_LOADED:
        emit("player.playing", nullptr);
        break;
    case MpvEventType::END_FILE_EOF:
    case MpvEventType::END_FILE_CANCEL:
        emit("player.stopped", nullptr);
        break;
    case MpvEventType::END_FILE_ERROR: {
        auto p = CefDictionaryValue::Create();
        p->SetString("message", ev.err_msg ? ev.err_msg : "Playback error");
        emit("player.error", p);
        emit("player.stopped", nullptr);
        break;
    }
    case MpvEventType::BUFFERED_RANGES: {
        auto ranges = CefListValue::Create();
        for (int i = 0; i < ev.range_count; i++) {
            auto r = CefDictionaryValue::Create();
            r->SetDouble("start", static_cast<double>(ev.ranges[i].start_ticks));
            r->SetDouble("end",   static_cast<double>(ev.ranges[i].end_ticks));
            ranges->SetDictionary(static_cast<size_t>(i), r);
        }
        auto p = CefDictionaryValue::Create();
        p->SetList("ranges", ranges);
        emit("player.bufferedRangesChanged", p);
        break;
    }
    default:
        break;
    }
}

}  // namespace player_router
