#pragma once

#include <string>
#include <string_view>

#include "../mpv/capabilities.h"

namespace jellyfin_device_profile {

// Translate mpv's reported capabilities into a Jellyfin device profile JSON
// document, suitable for injection into native-shim.js. Maps ffmpeg codec /
// demuxer names to Jellyfin's canonical names along the way.
//
// `force_transcode` makes the server transcode even when direct play would
// work (no video/audio DirectPlayProfiles emitted).
std::string Build(const mpv_capabilities::Capabilities& caps,
                  std::string_view device_name,
                  std::string_view app_version,
                  bool force_transcode);

// Process-local cache for the built profile JSON. Set once at startup in the
// browser process; read by WebBrowser::injectionProfile() so the JSON can be
// shipped to the renderer via extra_info. Empty if never set (unit-test
// builds, error paths).
void SetCachedJson(std::string json);
const std::string& CachedJson();

}  // namespace jellyfin_device_profile
