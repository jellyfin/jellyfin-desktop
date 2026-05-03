#pragma once

#include <string>
#include <vector>

#include <mpv/client.h>

namespace mpv_capabilities {

enum class MediaKind { Video, Audio, Subtitle };

struct Codec {
    std::string name;     // ffmpeg generic codec name (e.g. "h264", "subrip")
    MediaKind   kind;
};

struct Capabilities {
    std::vector<Codec>       decoders;  // classified video/audio/subtitle
    std::vector<std::string> demuxers;  // raw, comma-joined entries from demuxer-lavf-list
};

// Enumerate decoders directly from the linked libavcodec (classified by
// AVMediaType, deduped to one entry per AVCodecID), and query mpv for its
// demuxer list. mpv routes all decoding through libavcodec, so the codec
// set is identical to what mpv's `decoder-list` would report.
Capabilities Query(mpv_handle* mpv);

}  // namespace mpv_capabilities
