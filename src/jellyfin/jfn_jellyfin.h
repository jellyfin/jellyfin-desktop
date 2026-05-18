#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Kind discriminator for JfnCodec.
//   0 = Video, 1 = Audio, 2 = Subtitle
typedef struct {
    const char* name;
    uint8_t     kind;
} JfnCodec;

// Build the Jellyfin DeviceProfile JSON. Caller-owned strings; the Rust side
// only reads them. Returns a malloc'd, NUL-terminated UTF-8 string to be
// freed with jfn_jellyfin_free_string. Returns NULL on serialization failure.
char* jfn_jellyfin_build_device_profile(
    const JfnCodec*    decoders,
    size_t             n_decoders,
    const char* const* demuxers,
    size_t             n_demuxers,
    const char*        device_name,
    const char*        app_version,
    bool               force_transcode);

// Trim surrounding whitespace, lowercase Http:/Https: scheme prefix, and
// prepend http:// when no scheme is present. Returns malloc'd C string.
char* jfn_jellyfin_normalize_input(const char* input);

// Reduce a URL to its server base (truncate at "/web" or return origin).
// Returns malloc'd C string.
char* jfn_jellyfin_extract_base_url(const char* url);

// True iff `body` parses as a JSON object containing a non-empty string
// "Id" field. Used to validate a /System/Info/Public response.
bool jfn_jellyfin_is_valid_public_info(const char* body, size_t len);

// Process-local cache for the built device-profile JSON. Set once at startup
// in the browser process; read by WebBrowser when shipping extra_info to the
// renderer.
void  jfn_jellyfin_set_cached_profile(const char* json);
char* jfn_jellyfin_cached_profile(void);

void jfn_jellyfin_free_string(char* s);

#ifdef __cplusplus
}
#endif
