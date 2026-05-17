#pragma once

#include "event.h"
#include "jfn_playback.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Thin C++ facade over the Rust `jfn-playback` crate. All state and
// threading live behind the FFI; this header provides ergonomic wrappers
// and adapters that bridge the existing C++ sink interfaces (defined in
// event.h) to the Rust coordinator's vtable-based registration.

namespace playback_ffi {

inline std::string from_borrowed(const char* p, size_t len) {
    if (!p || len == 0) return {};
    return std::string(p, len);
}

inline MediaMetadata from_c(const JfnMediaMetadataC& m) {
    MediaMetadata out;
    out.id           = from_borrowed(m.id,           m.id_len);
    out.title        = from_borrowed(m.title,        m.title_len);
    out.artist       = from_borrowed(m.artist,       m.artist_len);
    out.album        = from_borrowed(m.album,        m.album_len);
    out.track_number = m.track_number;
    out.duration_us  = m.duration_us;
    out.art_url      = from_borrowed(m.art_url,      m.art_url_len);
    out.art_data_uri = from_borrowed(m.art_data_uri, m.art_data_uri_len);
    out.media_type   = static_cast<MediaType>(m.media_type);
    return out;
}

inline PlaybackSnapshot from_c(const JfnPlaybackSnapshotC& s) {
    PlaybackSnapshot out;
    out.presence = static_cast<PlayerPresence>(s.presence);
    out.phase = static_cast<PlaybackPhase>(s.phase);
    out.seeking = s.seeking;
    out.buffering = s.buffering;
    out.media_type = static_cast<MediaType>(s.media_type);
    out.position_us = s.position_us;
    out.variant_switch_pending = s.variant_switch_pending;
    out.rate = s.rate;
    out.duration_us = s.duration_us;
    out.fullscreen = s.fullscreen;
    out.maximized_before_fullscreen = s.maximized_before_fullscreen;
    out.layout_w = s.layout_w;
    out.layout_h = s.layout_h;
    out.pixel_w = s.pixel_w;
    out.pixel_h = s.pixel_h;
    out.display_hz = s.display_hz;
    if (s.buffered && s.buffered_len) {
        out.buffered.reserve(s.buffered_len);
        for (size_t i = 0; i < s.buffered_len; ++i) {
            out.buffered.push_back({s.buffered[i].start_ticks,
                                    s.buffered[i].end_ticks});
        }
    }
    return out;
}

inline PlaybackEvent from_c(const JfnPlaybackEventC& ev) {
    PlaybackEvent out;
    out.kind = static_cast<PlaybackEvent::Kind>(ev.kind);
    out.flag = ev.flag;
    out.error_message = from_borrowed(ev.error_message, ev.error_message_len);
    out.snapshot = from_c(ev.snapshot);
    out.metadata = from_c(ev.metadata);
    out.artwork_uri = from_borrowed(ev.artwork_uri, ev.artwork_uri_len);
    out.can_go_next = ev.can_go_next;
    out.can_go_prev = ev.can_go_prev;
    return out;
}

}  // namespace playback_ffi

extern "C" bool jfn_event_sink_thunk(void* ctx, const JfnPlaybackEventC* ev);
extern "C" bool jfn_action_sink_thunk(void* ctx, const JfnPlaybackActionC* act);

namespace playback {

inline void init() { jfn_playback_init(); }
inline void shutdown() { jfn_playback_shutdown(); }

inline PlaybackSnapshot snapshot() {
    JfnPlaybackSnapshotC s{};
    jfn_playback_snapshot(&s);
    return playback_ffi::from_c(s);
}

// Registers an event sink with the coordinator. Caller retains ownership
// of the shared_ptr; the coordinator stores the raw pointer for the
// lifetime of the program.
inline void register_event_sink(const std::shared_ptr<PlaybackEventSink>& sink) {
    jfn_playback_register_event_sink(sink.get(), &jfn_event_sink_thunk);
}

inline void register_action_sink(const std::shared_ptr<PlaybackActionSink>& sink) {
    jfn_playback_register_action_sink(sink.get(), &jfn_action_sink_thunk);
}

inline void post_file_loaded() { jfn_playback_post_file_loaded(); }
inline void post_load_starting(const std::string& item_id) {
    jfn_playback_post_load_starting(item_id.c_str());
}
inline void post_pause_changed(bool paused) { jfn_playback_post_pause_changed(paused); }
inline void post_end_file(EndReason reason, const std::string& err = {}) {
    jfn_playback_post_end_file(static_cast<uint8_t>(reason), err.c_str());
}
inline void post_seeking_changed(bool s) { jfn_playback_post_seeking_changed(s); }
inline void post_paused_for_cache(bool pfc) { jfn_playback_post_paused_for_cache(pfc); }
inline void post_core_idle(bool ci) { jfn_playback_post_core_idle(ci); }
inline void post_position(int64_t us) { jfn_playback_post_position(us); }
inline void post_media_type(MediaType t) {
    jfn_playback_post_media_type(static_cast<uint8_t>(t));
}
inline void post_video_frame_available(bool a) { jfn_playback_post_video_frame_available(a); }
inline void post_speed(double r) { jfn_playback_post_speed(r); }
inline void post_duration(int64_t us) { jfn_playback_post_duration(us); }
inline void post_fullscreen(bool fs, bool was_max) {
    jfn_playback_post_fullscreen(fs, was_max);
}
inline void post_osd_dims(int lw, int lh, int pw, int ph) {
    jfn_playback_post_osd_dims(lw, lh, pw, ph);
}
inline void post_buffered_ranges(const std::vector<PlaybackBufferedRange>& ranges) {
    static_assert(sizeof(PlaybackBufferedRange) == sizeof(JfnBufferedRange),
                  "PlaybackBufferedRange layout must match JfnBufferedRange");
    jfn_playback_post_buffered_ranges(
        reinterpret_cast<const JfnBufferedRange*>(ranges.data()), ranges.size());
}
inline void post_display_hz(double hz) { jfn_playback_post_display_hz(hz); }
inline void post_metadata(const MediaMetadata& m) {
    JfnMediaMetadataC c{};
    auto bind = [](const std::string& s) -> std::pair<const char*, size_t> {
        return s.empty() ? std::pair<const char*, size_t>{nullptr, 0}
                         : std::pair<const char*, size_t>{s.data(), s.size()};
    };
    auto [id_p, id_n]       = bind(m.id);
    auto [t_p, t_n]         = bind(m.title);
    auto [ar_p, ar_n]       = bind(m.artist);
    auto [al_p, al_n]       = bind(m.album);
    auto [url_p, url_n]     = bind(m.art_url);
    auto [data_p, data_n]   = bind(m.art_data_uri);
    c.id = id_p;            c.id_len = id_n;
    c.title = t_p;          c.title_len = t_n;
    c.artist = ar_p;        c.artist_len = ar_n;
    c.album = al_p;         c.album_len = al_n;
    c.track_number = m.track_number;
    c.duration_us = m.duration_us;
    c.art_url = url_p;      c.art_url_len = url_n;
    c.art_data_uri = data_p; c.art_data_uri_len = data_n;
    c.media_type = static_cast<uint8_t>(m.media_type);
    jfn_playback_post_metadata(&c);
}
inline void post_artwork(const std::string& data_uri) {
    jfn_playback_post_artwork(data_uri.c_str());
}
inline void post_queue_caps(bool can_next, bool can_prev) {
    jfn_playback_post_queue_caps(can_next, can_prev);
}
inline void post_seeked(int64_t us) { jfn_playback_post_seeked(us); }

}  // namespace playback

// RAII bracket used by run_with_cef to keep init/shutdown paired with the
// surrounding scope. Sinks must be registered between construction and
// the first post.
class PlaybackCoordinatorScope {
public:
    PlaybackCoordinatorScope();
    ~PlaybackCoordinatorScope();
};
