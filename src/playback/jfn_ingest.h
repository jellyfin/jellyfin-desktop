#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returned by jfn_playback_ingest_mpv_event as a bitfield.
//   bit 0 — MPV_EVENT_SHUTDOWN reached; caller should break its loop.
#define JFN_INGEST_FLAG_SHUTDOWN 0x1u

// Install the browser-side setScale thunk used to resolve
// DISPLAY_SCALE property changes. Replaces any prior callback.
void jfn_playback_set_display_scale_handler(void (*cb)(double));

// Provider + handler hooks consumed by the Rust-owned mpv event
// thread (jfn_playback_start_mpv_event_thread).
//
//   scale_provider    : returns device pixel scale (> 0)
//   macos_logical     : optional, fills *lw/*lh and returns true when
//                       a macOS logical-content-size override applies
//   fullscreen        : invoked on each `fullscreen` property change
//   shutdown          : invoked when MPV_EVENT_SHUTDOWN is observed
void jfn_playback_set_scale_provider(float (*cb)(void));
void jfn_playback_set_macos_logical_provider(bool (*cb)(int*, int*));
void jfn_playback_set_fullscreen_handler(void (*cb)(bool));
void jfn_playback_set_shutdown_handler(void (*cb)(void));

// Spawn / join the Rust-owned mpv event thread. The handle must be
// initialized (jfn_mpv_handle_init returned non-NULL) before start.
// Returns false if the handle is missing or the thread is already
// running. Stop is idempotent.
bool jfn_playback_start_mpv_event_thread(void);
void jfn_playback_stop_mpv_event_thread(void);

// Decode one raw mpv_event* (returned by mpv_wait_event) into
// coordinator inputs + side-channel callbacks. Returns flag bits.
//
// `has_macos_logical` non-zero signals that mac_lw/mac_lh carry a
// valid macOS logical-content size override. Non-macOS callers pass
// false / zeros.
uint8_t jfn_playback_ingest_mpv_event(
    const void* ev,           // mpv_event*
    float scale,
    bool has_macos_logical,
    int  mac_lw,
    int  mac_lh);

// Push synthetic OSD-dim pixels through the same digest path the
// `osd-dimensions` property observation drives. Used by the Wayland
// xdg_toplevel.configure intercept.
void jfn_playback_post_osd_pixels(
    int   pw,
    int   ph,
    float scale,
    bool  has_macos_logical,
    int   mac_lw,
    int   mac_lh);

// State accessors mirroring the legacy `mpv::*` getters that the C++
// side used to read from `s_*` atomics in src/mpv/event.cpp.
bool   jfn_playback_fullscreen(void);
bool   jfn_playback_window_maximized(void);
int    jfn_playback_osd_pw(void);
int    jfn_playback_osd_ph(void);
int    jfn_playback_window_pw(void);
int    jfn_playback_window_ph(void);
double jfn_playback_display_scale(void);
double jfn_playback_display_hz(void);
void   jfn_playback_set_display_hz(double hz);
void   jfn_playback_set_window_pixels(int pw, int ph);

#ifdef __cplusplus
}
#endif
