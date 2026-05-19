#pragma once

// C ABI exposing the post-init mpv handle. Replaces the legacy
// MpvHandle wrapper in src/mpv/handle.h. All entry points borrow the
// global handle published by jfn_mpv_handle_init; they no-op silently
// if the handle is missing.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pulls in the lifecycle FFI (jfn_mpv_handle_init / _get / _terminate).
#include "jfn_mpv_boot.h"

struct mpv_event;

#ifdef __cplusplus
extern "C" {
#endif

// =========================================================================
// Generic property R/W + command (async; reply_userdata = 0)
// =========================================================================

void jfn_mpv_set_property_flag_async(const char* name, bool value);
void jfn_mpv_set_property_double_async(const char* name, double value);
void jfn_mpv_set_property_int_async(const char* name, int64_t value);
void jfn_mpv_set_property_string_async(const char* name, const char* value);

// Sync int property read. Returns libmpv's error code (0 on success).
int   jfn_mpv_get_property_int(const char* name, int64_t* out);
// Sync string read. Returns malloc'd UTF-8 string; free with jfn_mpv_free_string.
char* jfn_mpv_get_property_string(const char* name);
void  jfn_mpv_free_string(char* s);

// Async command. args is a const char*[n] table (no NULL terminator
// required; wrapper appends one). No-op on NULL entries.
void  jfn_mpv_command_async(const char* const* args, size_t n);

// =========================================================================
// Event drain
// =========================================================================

struct mpv_event* jfn_mpv_wait_event(double timeout);
void              jfn_mpv_wakeup(void);

// =========================================================================
// Player API
// =========================================================================

void jfn_mpv_play(void);
void jfn_mpv_pause(void);
void jfn_mpv_toggle_pause(void);
void jfn_mpv_stop(void);
void jfn_mpv_seek_absolute(double secs);
void jfn_mpv_set_volume(double v);
void jfn_mpv_set_muted(bool v);
void jfn_mpv_set_speed(double v);
void jfn_mpv_set_audio_delay(double secs);
void jfn_mpv_set_subtitle_delay(double secs);
void jfn_mpv_set_start_position(double secs);

// Track id sentinel: 0 disables; >=1 selects an explicit mpv track id.
// mpv auto-track-selection is globally off (track-auto-selection=no);
// jellyfin-web is the sole authority.
void jfn_mpv_set_audio_track(int64_t id);
void jfn_mpv_set_subtitle_track(int64_t id);

void jfn_mpv_sub_add(const char* url);
void jfn_mpv_audio_add(const char* url);

// =========================================================================
// LoadFile + deferred track selection
// =========================================================================

typedef struct {
    double  start_secs;
    int64_t video_track;
    int64_t audio_track;
    int64_t sub_track;
    const char* external_audio_url;   // may be NULL
    const char* external_sub_url;     // may be NULL
    // MediaSourceInfo.IsInfiniteStream. When true AND audio_track==0,
    // unprobed live TV — let mpv's per-format demuxer pick audio
    // (HLS DEFAULT=YES, MPEG-TS first PMT, etc.) instead of silencing.
    bool        is_infinite_stream;
} JfnMpvLoadOptions;

void jfn_mpv_load_file(const char* path, const JfnMpvLoadOptions* opts);

// Apply pending vid/aid/sid + external streams + unpause. Call from the
// FILE_LOADED event handler.
void jfn_mpv_apply_pending_track_selection_and_play(void);

// Aspect mode: "auto" | "cover" | "fill". Unknown modes are ignored.
void jfn_mpv_set_aspect_mode(const char* mode);

// =========================================================================
// Window / display
// =========================================================================

void jfn_mpv_set_fullscreen(bool v);
void jfn_mpv_toggle_fullscreen(void);
void jfn_mpv_set_window_minimized(bool v);
void jfn_mpv_set_window_maximized(bool v);
void jfn_mpv_set_force_window_position(bool v);
void jfn_mpv_set_geometry(const char* geom);

// Background color helpers — pass / return packed 0x00RRGGBB.
void     jfn_mpv_set_background_color_hex(const char* hex);
uint32_t jfn_mpv_get_background_color(void);

#ifdef __cplusplus
}
#endif
