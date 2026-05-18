#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Window geometry shared with Rust. Layout must match
// jfn_config::JfnWindowGeometry.
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t logical_width;
    int32_t logical_height;
    float   scale;
    bool    maximized;
} JfnWindowGeometry;

// One-shot init with the on-disk config file path. Idempotent.
void jfn_settings_init(const char* path);

bool jfn_settings_load(void);
bool jfn_settings_save(void);
void jfn_settings_save_async(void);

// String getters: returned pointer is heap-allocated; caller frees with
// jfn_settings_free_string. Returns a non-null pointer (possibly empty
// string) for known fields.
char* jfn_settings_get_server_url(void);
char* jfn_settings_get_hwdec(void);
char* jfn_settings_get_audio_passthrough(void);
char* jfn_settings_get_audio_channels(void);
char* jfn_settings_get_log_level(void);
char* jfn_settings_get_device_name(void);

void jfn_settings_set_server_url(const char* v);
void jfn_settings_set_hwdec(const char* v);
void jfn_settings_set_audio_passthrough(const char* v);
void jfn_settings_set_audio_channels(const char* v);
void jfn_settings_set_log_level(const char* v);
// Trims whitespace, truncates to 64 chars, and clears the override if it
// matches `platform_default`.
void jfn_settings_set_device_name(const char* v, const char* platform_default);

bool jfn_settings_get_audio_exclusive(void);
bool jfn_settings_get_disable_gpu_compositing(void);
bool jfn_settings_get_titlebar_theme_color(void);
bool jfn_settings_get_transparent_titlebar(void);
bool jfn_settings_get_force_transcoding(void);

void jfn_settings_set_audio_exclusive(bool v);
void jfn_settings_set_disable_gpu_compositing(bool v);
void jfn_settings_set_titlebar_theme_color(bool v);
void jfn_settings_set_transparent_titlebar(bool v);
void jfn_settings_set_force_transcoding(bool v);

void jfn_settings_get_window_geometry(JfnWindowGeometry* out);
void jfn_settings_set_window_geometry(const JfnWindowGeometry* in_);

// Build the CLI-equivalent JSON string injected into JS. Caller frees with
// jfn_settings_free_string. platform_default is the platform's hostname-
// derived device name. hwdec_opts is an array of `n_opts` C strings; each
// becomes an entry in the `hwdecOptions` JSON array.
char* jfn_settings_cli_json(const char* platform_default,
                            const char* const* hwdec_opts,
                            size_t n_opts);

void jfn_settings_free_string(char* s);

#ifdef __cplusplus
}
#endif
