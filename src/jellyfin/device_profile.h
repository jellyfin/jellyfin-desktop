#pragma once

#include "jfn_jellyfin.h"

#include "../mpv/capabilities.h"

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
inline std::string Build(const mpv_capabilities::Capabilities& caps,
                         std::string_view device_name,
                         std::string_view app_version,
                         bool force_transcode) {
    using mpv_capabilities::MediaKind;

    std::vector<JfnCodec> codec_arr;
    codec_arr.reserve(caps.decoders.size());
    for (const auto& c : caps.decoders) {
        uint8_t kind = 0;
        switch (c.kind) {
        case MediaKind::Video:    kind = 0; break;
        case MediaKind::Audio:    kind = 1; break;
        case MediaKind::Subtitle: kind = 2; break;
        }
        codec_arr.push_back({c.name.c_str(), kind});
    }

    std::vector<const char*> demuxer_ptrs;
    demuxer_ptrs.reserve(caps.demuxers.size());
    for (const auto& d : caps.demuxers) {
        demuxer_ptrs.push_back(d.c_str());
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
