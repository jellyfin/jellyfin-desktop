#pragma once

#include "jfn_jellyfin.h"

#include "../mpv/jfn_mpv_boot.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Header-only thin wrapper over the Rust device-profile builder in
// src/jellyfin/src/lib.rs.

namespace jellyfin_device_profile {

// Translate mpv's reported capabilities into a Jellyfin device profile JSON
// document, suitable for injection into native-shim.js. Maps ffmpeg codec /
// demuxer names to Jellyfin's canonical names along the way.
//
// `force_transcode` makes the server transcode even when direct play would
// work (no video/audio DirectPlayProfiles emitted).
inline std::string Build(const JfnMpvCapabilities* caps,
                         std::string_view device_name,
                         std::string_view app_version,
                         bool force_transcode) {
    size_t n_dec = jfn_mpv_capabilities_decoder_count(caps);
    std::vector<JfnCodec> codec_arr;
    codec_arr.reserve(n_dec);
    for (size_t i = 0; i < n_dec; i++) {
        codec_arr.push_back({
            jfn_mpv_capabilities_decoder_name(caps, i),
            jfn_mpv_capabilities_decoder_kind(caps, i),
        });
    }

    size_t n_dem = jfn_mpv_capabilities_demuxer_count(caps);
    std::vector<const char*> demuxer_ptrs;
    demuxer_ptrs.reserve(n_dem);
    for (size_t i = 0; i < n_dem; i++) {
        demuxer_ptrs.push_back(jfn_mpv_capabilities_demuxer_name(caps, i));
    }

    const std::string name(device_name);
    const std::string version(app_version);

    char* raw = jfn_jellyfin_build_device_profile(
        codec_arr.data(), codec_arr.size(),
        demuxer_ptrs.data(), demuxer_ptrs.size(),
        name.c_str(),
        version.c_str(),
        force_transcode);
    std::string json = raw ? std::string(raw) : std::string();
    jfn_jellyfin_free_string(raw);
    return json;
}

// Process-local cache for the built profile JSON. Set once at startup in the
// browser process; read by WebBrowser::injectionProfile() so the JSON can be
// shipped to the renderer via extra_info. Empty if never set (unit-test
// builds, error paths).
inline void SetCachedJson(const std::string& json) {
    jfn_jellyfin_set_cached_profile(json.c_str());
}

inline std::string CachedJson() {
    char* raw = jfn_jellyfin_cached_profile();
    std::string out = raw ? std::string(raw) : std::string();
    jfn_jellyfin_free_string(raw);
    return out;
}

}  // namespace jellyfin_device_profile
