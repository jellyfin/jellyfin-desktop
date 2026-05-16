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

void jfn_jellyfin_free_string(char* s);

#ifdef __cplusplus
}
#endif
