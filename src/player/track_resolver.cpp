#include "track_resolver.h"

namespace track_resolver {

namespace {

// Walk type-filtered, non-external streams in declaration order to find a
// 1-based relative index matching `jellyfin_index`. Returns 0 if not found.
int relative_index_by_type(const std::vector<MediaStream>& streams,
                           int jellyfin_index,
                           const std::string& stream_type) {
    int rel = 1;
    for (const auto& s : streams) {
        if (s.type != stream_type || s.is_external) continue;
        if (s.index == jellyfin_index) return rel;
        ++rel;
    }
    return 0;
}

const MediaStream* find_by_index(const std::vector<MediaStream>& streams,
                                 int jellyfin_index) {
    for (const auto& s : streams) {
        if (s.index == jellyfin_index) return &s;
    }
    return nullptr;
}

}  // namespace

int64_t resolve_audio_aid(const std::vector<MediaStream>& streams,
                          std::optional<int> jellyfin_index) {
    if (!jellyfin_index || *jellyfin_index < 0) return kTrackAuto;
    int rel = relative_index_by_type(streams, *jellyfin_index, "Audio");
    return rel == 0 ? kTrackAuto : static_cast<int64_t>(rel);
}

SubtitleSelection resolve_subtitle(const std::vector<MediaStream>& streams,
                                   std::optional<int> jellyfin_index,
                                   SubtitleUnresolvedFallback fallback) {
    SubtitleSelection out;
    if (!jellyfin_index || *jellyfin_index < 0) {
        out.kind = SubtitleSelection::Kind::Disable;
        return out;
    }

    if (const MediaStream* s = find_by_index(streams, *jellyfin_index)) {
        if (s->delivery_method == "External" && !s->delivery_url.empty()) {
            out.kind = SubtitleSelection::Kind::External;
            out.url = s->delivery_url;
            return out;
        }
    }

    int rel = relative_index_by_type(streams, *jellyfin_index, "Subtitle");
    if (rel != 0) {
        out.kind = SubtitleSelection::Kind::Embedded;
        out.sid = rel;
        return out;
    }

    if (fallback == SubtitleUnresolvedFallback::AutoFallback) {
        out.kind = SubtitleSelection::Kind::Embedded;
        out.sid = kTrackAuto;
    }  // else: Disable (default)
    return out;
}

}  // namespace track_resolver
