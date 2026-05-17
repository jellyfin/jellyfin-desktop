#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================
// Mirror types for FFI delivery.
// String fields are (pointer, length) pairs; the pointer may be NULL when
// length is zero. Lifetimes equal the duration of the try_post call.
// =====================================================================

typedef struct {
    int64_t start_ticks;
    int64_t end_ticks;
} JfnBufferedRange;

typedef struct {
    uint8_t  presence;     // 0=None 1=Present
    uint8_t  phase;        // 0=Starting 1=Playing 2=Paused 3=Stopped
    bool     seeking;
    bool     buffering;
    uint8_t  media_type;   // 0=Unknown 1=Audio 2=Video
    int64_t  position_us;
    bool     variant_switch_pending;
    double   rate;
    int64_t  duration_us;
    bool     fullscreen;
    bool     maximized_before_fullscreen;
    int32_t  layout_w;
    int32_t  layout_h;
    int32_t  pixel_w;
    int32_t  pixel_h;
    double   display_hz;
    const JfnBufferedRange* buffered;
    size_t   buffered_len;
} JfnPlaybackSnapshotC;

typedef struct {
    const char* id;            size_t id_len;
    const char* title;         size_t title_len;
    const char* artist;        size_t artist_len;
    const char* album;         size_t album_len;
    int32_t     track_number;
    int64_t     duration_us;
    const char* art_url;       size_t art_url_len;
    const char* art_data_uri;  size_t art_data_uri_len;
    uint8_t     media_type;
} JfnMediaMetadataC;

typedef struct {
    // PlaybackEventKind:
    //   0 Started        1 Paused          2 Finished     3 Canceled
    //   4 Error          5 SeekingChanged  6 BufferingChanged
    //   7 MediaTypeChanged  8 TrackLoaded  9 PositionChanged
    //  10 DurationChanged 11 RateChanged  12 FullscreenChanged
    //  13 OsdDimsChanged 14 BufferedRangesChanged
    //  15 DisplayHzChanged 16 MetadataChanged 17 ArtworkChanged
    //  18 QueueCapsChanged 19 Seeked
    uint8_t              kind;
    bool                 flag;
    const char*          error_message;
    size_t               error_message_len;
    JfnPlaybackSnapshotC snapshot;
    JfnMediaMetadataC    metadata;
    const char*          artwork_uri;
    size_t               artwork_uri_len;
    bool                 can_go_next;
    bool                 can_go_prev;
} JfnPlaybackEventC;

typedef struct {
    uint8_t kind;  // 0 = ApplyPendingTrackSelectionAndPlay
} JfnPlaybackActionC;

// =====================================================================
// Lifecycle + producers
// =====================================================================

void jfn_playback_init(void);
void jfn_playback_shutdown(void);

// Sink registration. `ctx` is opaque to Rust and passed back to try_post
// on each delivery. Must be called between init() and the first post.
void jfn_playback_register_event_sink(
    void* ctx,
    bool (*try_post)(void* ctx, const JfnPlaybackEventC* ev));
void jfn_playback_register_action_sink(
    void* ctx,
    bool (*try_post)(void* ctx, const JfnPlaybackActionC* act));

// Copy the current snapshot. The buffered pointer is zeroed on return
// (only inline POD fields are usable).
void jfn_playback_snapshot(JfnPlaybackSnapshotC* out);

void jfn_playback_post_file_loaded(void);
void jfn_playback_post_load_starting(const char* item_id);  // NUL-terminated, may be NULL
void jfn_playback_post_pause_changed(bool paused);
void jfn_playback_post_end_file(uint8_t reason, const char* error_message);
void jfn_playback_post_seeking_changed(bool seeking);
void jfn_playback_post_paused_for_cache(bool pfc);
void jfn_playback_post_core_idle(bool ci);
void jfn_playback_post_position(int64_t position_us);
void jfn_playback_post_media_type(uint8_t ty);
void jfn_playback_post_video_frame_available(bool available);
void jfn_playback_post_speed(double rate);
void jfn_playback_post_duration(int64_t duration_us);
void jfn_playback_post_fullscreen(bool fullscreen, bool was_maximized);
void jfn_playback_post_osd_dims(int32_t lw, int32_t lh, int32_t pw, int32_t ph);
void jfn_playback_post_buffered_ranges(const JfnBufferedRange* ranges, size_t len);
void jfn_playback_post_display_hz(double hz);
void jfn_playback_post_metadata(const JfnMediaMetadataC* m);
void jfn_playback_post_artwork(const char* data_uri);
void jfn_playback_post_queue_caps(bool can_go_next, bool can_go_prev);
void jfn_playback_post_seeked(int64_t position_us);

// =====================================================================
// MPRIS projection (Linux MPRIS Player interface derivation rules)
// =====================================================================

typedef struct {
    uint8_t status;            // 0=Stopped 1=Playing 2=Paused
    bool    can_play;
    bool    can_pause;
    bool    can_seek;
    bool    can_control;
    bool    metadata_active;   // false -> caller substitutes empty metadata
    double  rate;
} JfnMprisDerivedC;

void jfn_mpris_project(
    uint8_t phase,                    // PlaybackPhase discriminant
    bool seeking,
    bool buffering,
    int64_t metadata_duration_us,     // from MprisContent.metadata, not snapshot
    double pending_rate,
    JfnMprisDerivedC* out);

#ifdef __cplusplus
}
#endif
