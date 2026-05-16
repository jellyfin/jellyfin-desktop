#include "device_profile.h"

#include <string>
#include <vector>

#include "jfn_jellyfin.h"

#include "../logging.h"

namespace jellyfin_device_profile {

std::string Build(const mpv_capabilities::Capabilities& caps,
                  std::string_view device_name,
                  std::string_view app_version,
                  bool force_transcode) {
    using mpv_capabilities::MediaKind;

    // Build flat C arrays for the Rust ABI. All pointers reference storage
    // owned by `caps` / `device_name` / `app_version` for the duration of
    // this synchronous call.
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

    LOG_INFO(LOG_MAIN, "Device profile: {}", json);
    return json;
}

namespace {
std::string g_cached_json;
}

void SetCachedJson(std::string json) { g_cached_json = std::move(json); }
const std::string& CachedJson() { return g_cached_json; }

}  // namespace jellyfin_device_profile
