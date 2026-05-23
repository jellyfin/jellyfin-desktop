#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Window-scoped theme color tracker. Implementation in jfn-color
// (src/color/src/theme.rs). One process-wide instance; init once after
// platform startup, before any browser creates.
//
// `on_set_theme_color` may be null when titlebarThemeColor is disabled.
// `on_set_bg_hex` is required and receives a NUL-terminated `#RRGGBB`
// pointer valid for the duration of the call.
void jfn_theme_color_init(
    void (*on_set_theme_color)(uint32_t rgb),
    void (*on_set_bg_hex)(const char* hex_cstr));

// Update the video-mode background. Captured from mpv after startup.
void jfn_theme_color_set_video_bg(uint32_t rgb);

// Renderer fired a `<meta name="theme-color">` update.
void jfn_theme_color_on_color(uint32_t rgb);

// Loading overlay dismissed; buffered theme color is now allowed to apply.
void jfn_theme_color_on_overlay_dismissed(void);

// Playback transitioned in/out of video mode; resolved color flips to mpv
// bg while active.
void jfn_theme_color_set_video_mode(bool active);

// Drop the singleton; subsequent calls are no-ops until reinitialised.
void jfn_theme_color_shutdown(void);

#ifdef __cplusplus
}
#endif
