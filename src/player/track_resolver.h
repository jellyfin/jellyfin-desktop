#pragma once

// Pure translation between Jellyfin's MediaStream view of tracks and mpv's
// internal track numbering. Kept free of CEF so the logic can be unit-tested
// directly with doctest.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace track_resolver {

// Subset of Jellyfin's MediaStream the resolver actually consults.
struct MediaStream {
    int index = 0;                    // Jellyfin's global stream index
    std::string type;                 // "Video" / "Audio" / "Subtitle"
    bool is_external = false;         // skipped during relative-index walk
    std::string delivery_method;      // "External" routes via sub-add URL
    std::string delivery_url;
};

// mpv conventions; mirror MpvHandle::kTrackAuto / kTrackDisable.
inline constexpr int64_t kTrackAuto    = -1;
inline constexpr int64_t kTrackDisable =  0;

// Audio track resolution.
//
// missing/null  -> kTrackAuto
// found         -> mpv 1-based aid (relative within audio streams)
// not found     -> kTrackAuto (let mpv decide)
int64_t resolve_audio_aid(const std::vector<MediaStream>& streams,
                          std::optional<int> jellyfin_index);

struct SubtitleSelection {
    enum class Kind { Disable, External, Embedded };
    Kind kind = Kind::Disable;
    int64_t sid = kTrackDisable;  // when Embedded
    std::string url;              // when External
};

// Behavior when the requested index doesn't resolve to an embedded subtitle:
//   AutoFallback   -> Embedded { sid = kTrackAuto } (matches load() path)
//   DisableFallback-> Disable                       (matches setSubtitleStreamIndex path)
enum class SubtitleUnresolvedFallback { AutoFallback, DisableFallback };

// Subtitle resolution.
//
// missing/null  -> Disable
// external      -> External { url }
// found embed   -> Embedded { sid = relative index }
// not found     -> per `fallback`
SubtitleSelection resolve_subtitle(const std::vector<MediaStream>& streams,
                                   std::optional<int> jellyfin_index,
                                   SubtitleUnresolvedFallback fallback);

}  // namespace track_resolver
