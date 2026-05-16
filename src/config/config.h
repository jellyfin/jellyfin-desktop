#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Flat data struct shared with C++. String fields are owned by the caller that
// allocates them. After jfn_config_load() populates strings, free the struct
// with jfn_config_free_data(). When passing a struct to jfn_config_save() or
// jfn_config_cli_json(), the caller retains ownership of its string buffers;
// the Rust side never frees inputs.
typedef struct {
    char* server_url;
    char* hwdec;
    char* audio_passthrough;
    char* audio_channels;
    char* log_level;
    char* device_name;

    int32_t window_x;
    int32_t window_y;
    int32_t window_width;
    int32_t window_height;
    int32_t window_logical_width;
    int32_t window_logical_height;
    float window_scale;
    bool window_maximized;

    bool audio_exclusive;
    bool disable_gpu_compositing;
    bool titlebar_theme_color;
    bool transparent_titlebar;
    bool force_transcoding;
} JfnConfigData;

// Initialize a JfnConfigData to defaults (all strings NULL, numeric defaults
// matching the C++ Settings constructor).
void jfn_config_init_defaults(JfnConfigData* d);

// Parse JSON file at path into out. Fields absent from JSON keep their
// initial values from `out` (caller must call jfn_config_init_defaults
// first if they want defaults). Returns false if the file is missing or
// invalid JSON; in that case `out` is unchanged.
bool jfn_config_load(const char* path, JfnConfigData* out);

// Serialize `in` to JSON and write atomically to path. `hwdec_default` is the
// sentinel value that suppresses writing the hwdec key (e.g. "no"). Returns
// false on I/O error.
bool jfn_config_save(const char* path, const JfnConfigData* in, const char* hwdec_default);

// Free strings stored inside `d` by jfn_config_load(). Sets each char* to NULL.
void jfn_config_free_data(JfnConfigData* d);

// Build the CLI-equivalent JSON string injected into JS. Caller frees with
// jfn_config_free_string. platform_default is the platform's hostname-derived
// device name. hwdec_opts is an array of `n_opts` C strings; each becomes an
// entry in the `hwdecOptions` JSON array.
char* jfn_config_cli_json(const JfnConfigData* in,
                          const char* platform_default,
                          const char* const* hwdec_opts,
                          size_t n_opts);

// Free a string returned by jfn_config_cli_json.
void jfn_config_free_string(char* s);

// Validate a Jellyfin /System/Info/Public response body: returns true iff the
// body is a JSON object containing a non-empty string `Id` field.
bool jfn_jellyfin_is_valid_public_info(const char* body, size_t len);

#ifdef __cplusplus
}
#endif
