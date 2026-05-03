#include "capabilities.h"

#include <unordered_set>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "../logging.h"

namespace mpv_capabilities {

namespace {

void enumerate_decoders(std::vector<Codec>& out) {
    // Iterate every codec compiled into the linked libavcodec, keep decoders,
    // and dedupe by AVCodecID — multiple AVCodec entries (e.g. h264, h264_qsv,
    // h264_v4l2m2m) share the same id and resolve to the same generic name
    // via avcodec_get_name(). Jellyfin matches against ffprobe-derived generic
    // names, so the wrapper-specific AVCodec::name is the wrong granularity.
    std::unordered_set<int> seen;
    void* iter = nullptr;
    while (const AVCodec* codec = av_codec_iterate(&iter)) {
        if (!av_codec_is_decoder(codec)) continue;
        MediaKind kind;
        switch (codec->type) {
        case AVMEDIA_TYPE_VIDEO:    kind = MediaKind::Video; break;
        case AVMEDIA_TYPE_AUDIO:    kind = MediaKind::Audio; break;
        case AVMEDIA_TYPE_SUBTITLE: kind = MediaKind::Subtitle; break;
        default: continue;
        }
        if (!seen.insert(static_cast<int>(codec->id)).second) continue;
        const char* name = avcodec_get_name(codec->id);
        if (!name || !*name) continue;
        out.push_back({std::string(name), kind});
    }
}

void parse_string_list(const mpv_node* root, std::vector<std::string>& out) {
    if (!root || root->format != MPV_FORMAT_NODE_ARRAY || !root->u.list) return;
    for (int i = 0; i < root->u.list->num; i++) {
        const mpv_node& v = root->u.list->values[i];
        if (v.format != MPV_FORMAT_STRING || !v.u.string) continue;
        out.emplace_back(v.u.string);
    }
}

struct NodeGuard {
    mpv_node node{};
    ~NodeGuard() { mpv_free_node_contents(&node); }
};

}  // namespace

Capabilities Query(mpv_handle* mpv) {
    Capabilities caps;
    enumerate_decoders(caps.decoders);
    if (!mpv) return caps;

    NodeGuard g;
    if (mpv_get_property(mpv, "demuxer-lavf-list", MPV_FORMAT_NODE, &g.node) == 0)
        parse_string_list(&g.node, caps.demuxers);
    else
        LOG_WARN(LOG_MAIN, "mpv_get_property(demuxer-lavf-list) failed");

    return caps;
}

}  // namespace mpv_capabilities
