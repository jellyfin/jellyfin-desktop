#include "device_profile.h"

#include <string_view>
#include <unordered_map>
#include <vector>

#include "include/cef_parser.h"
#include "include/cef_values.h"

#include "../logging.h"

namespace jellyfin_device_profile {

namespace {

// Jellyfin source pinned to commit 2c62d40 (matches the third_party/jellyfin
// submodule). Profile-vs-stream matching is plain case-insensitive equality
// against ffprobe-derived names, so any rename below has to mirror what the
// server stores on MediaSource.Container / MediaStream.Codec at probe time.
//
// Match logic:
//   https://github.com/jellyfin/jellyfin/blob/2c62d40f0d13926874eef9118a95be0dcee4e659/MediaBrowser.Model/Extensions/ContainerHelper.cs#L82-L107
// Subtitle match in StreamBuilder:
//   https://github.com/jellyfin/jellyfin/blob/2c62d40f0d13926874eef9118a95be0dcee4e659/MediaBrowser.Model/Dlna/StreamBuilder.cs#L1476
// Container normalization at probe time (NormalizeFormat):
//   https://github.com/jellyfin/jellyfin/blob/2c62d40f0d13926874eef9118a95be0dcee4e659/MediaBrowser.MediaEncoding/Probing/ProbeResultNormalizer.cs#L270-L315
// Subtitle normalization at probe time (NormalizeSubtitleCodec):
//   https://github.com/jellyfin/jellyfin/blob/2c62d40f0d13926874eef9118a95be0dcee4e659/MediaBrowser.MediaEncoding/Probing/ProbeResultNormalizer.cs#L632-L652

using RenameMap = std::unordered_map<std::string_view, std::string_view>;

const RenameMap kSubtitleRenames = {
    {"subrip",            "srt"},
    {"ass",               "ssa"},
    {"hdmv_pgs_subtitle", "PGSSUB"},
    {"dvd_subtitle",      "DVDSUB"},
    {"dvb_subtitle",      "DVBSUB"},
    {"dvb_teletext",      "DVBTXT"},
};

const RenameMap kContainerRenames = {
    {"matroska",  "mkv"},
    {"mpegts",    "ts"},
    {"mpegvideo", "mpeg"},
};

std::string join_csv(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); i++) {
        if (i) out += ',';
        out += items[i];
    }
    return out;
}

CefRefPtr<CefDictionaryValue> make_subtitle_profile(std::string_view format,
                                                    const char* method) {
    auto d = CefDictionaryValue::Create();
    d->SetString("Format", std::string(format));
    d->SetString("Method", method);
    return d;
}

// Filter `items` down to entries present in `allowed`, preserving the order
// of `items` (Jellyfin treats this as the server's preference order).
std::vector<std::string> filter_in_order(const std::vector<std::string>& items,
                                         const std::vector<std::string>& allowed) {
    std::vector<std::string> out;
    for (const auto& item : items) {
        for (const auto& a : allowed) {
            if (a == item) {
                out.push_back(item);
                break;
            }
        }
    }
    return out;
}

// Expand each input through `renames` and return the deduped union of raw +
// renamed names. Inputs may be comma-joined ffmpeg aliases (e.g.
// "matroska,webm"); each piece is split before lookup.
std::vector<std::string> expand_with_renames(const std::vector<std::string>& inputs,
                                             const RenameMap& renames) {
    std::vector<std::string> out;
    std::unordered_map<std::string, bool> seen;
    auto add = [&](std::string_view s) {
        if (s.empty()) return;
        std::string str(s);
        if (seen.emplace(str, true).second)
            out.push_back(std::move(str));
    };
    for (const auto& input : inputs) {
        size_t pos = 0;
        while (pos <= input.size()) {
            size_t comma = input.find(',', pos);
            if (comma == std::string::npos) comma = input.size();
            std::string_view piece(input.data() + pos, comma - pos);
            add(piece);
            auto it = renames.find(piece);
            if (it != renames.end()) add(it->second);
            if (comma == input.size()) break;
            pos = comma + 1;
        }
    }
    return out;
}

}  // namespace

std::string Build(const mpv_capabilities::Capabilities& caps,
                  std::string_view device_name,
                  std::string_view app_version,
                  bool force_transcode) {
    using mpv_capabilities::MediaKind;

    // Hardcoded transcoding fallback. These describe what the server should
    // produce when direct play isn't possible; the codec lists are the
    // server's preference order.
    static constexpr const char* kTranscodeContainer        = "ts";
    static constexpr const char* kTranscodeProtocol         = "hls";
    static constexpr const char* kTranscodeMaxAudioChannels = "6";
    // Codec sets come from Jellyfin's StreamBuilder._supportedHls* lists
    // (alac dropped to stay under the server's 40-char AudioCodec query-param
    // validator, ^[a-zA-Z0-9\-\._,|]{0,40}$):
    // https://github.com/jellyfin/jellyfin/blob/2c62d40f0d13926874eef9118a95be0dcee4e659/MediaBrowser.Model/Dlna/StreamBuilder.cs#L31-L33
    // Order is our preference: best compression/quality first so the server
    // picks the most efficient target it can produce.
    static const std::vector<std::string> kTranscodeVideoCodec =
        {"av1", "hevc", "h264", "vp9"};
    static const std::vector<std::string> kTranscodeAudioCodecMp4 =
        {"opus", "aac", "eac3", "ac3", "flac", "mp3", "dts", "truehd"};
    static const std::vector<std::string> kTranscodeAudioCodecTs =
        {"aac", "eac3", "ac3", "mp3"};

    // Bucket decoders by kind.
    std::vector<std::string> video_codecs, audio_codecs, subtitle_codecs;
    for (const auto& c : caps.decoders) {
        switch (c.kind) {
        case MediaKind::Video:    video_codecs.push_back(c.name); break;
        case MediaKind::Audio:    audio_codecs.push_back(c.name); break;
        case MediaKind::Subtitle: subtitle_codecs.push_back(c.name); break;
        }
    }
    const std::string video_csv = force_transcode ? "" : join_csv(video_codecs);
    const std::string audio_csv = force_transcode ? "" : join_csv(audio_codecs);

    auto containers     = expand_with_renames(caps.demuxers,   kContainerRenames);
    auto subtitle_names = expand_with_renames(subtitle_codecs, kSubtitleRenames);

    // TranscodingProfiles tell the server which formats it may transcode TO,
    // not what we can decode. Stick to the curated preference list intersected
    // with mpv's decoder support — adding the rest would (a) invite the server
    // to transcode to formats we don't want as targets (truehd, dts, vp9...)
    // and (b) push the AudioCodec CSV past the server's 40-char query-param
    // validator (^[a-zA-Z0-9\-\._,|]{0,40}$).
    const std::string transcode_video_csv =
        join_csv(filter_in_order(kTranscodeVideoCodec, video_codecs));
    const std::string transcode_audio_csv_mp4 =
        join_csv(filter_in_order(kTranscodeAudioCodecMp4, audio_codecs));
    const std::string transcode_audio_csv_ts =
        join_csv(filter_in_order(kTranscodeAudioCodecTs, audio_codecs));

    // DirectPlayProfiles. ContainerHelper.ContainsContainer splits both the
    // profile's Container and the file's MediaSource.Container on comma and
    // does case-insensitive equality, so one entry with every container
    // comma-joined matches identically to N entries with one container each —
    // without repeating the codec CSV per container.
    const std::string container_csv = join_csv(containers);
    auto direct_play = CefListValue::Create();
    size_t dp_idx = 0;
    if (!video_csv.empty() || force_transcode) {
        auto entry = CefDictionaryValue::Create();
        entry->SetString("Container", container_csv);
        entry->SetString("Type", "Video");
        entry->SetString("VideoCodec", video_csv);
        entry->SetString("AudioCodec", audio_csv);
        direct_play->SetDictionary(dp_idx++, entry);
    }
    if (!audio_csv.empty() || force_transcode) {
        auto entry = CefDictionaryValue::Create();
        entry->SetString("Container", container_csv);
        entry->SetString("Type", "Audio");
        entry->SetString("AudioCodec", audio_csv);
        direct_play->SetDictionary(dp_idx++, entry);
    }
    {
        auto photo = CefDictionaryValue::Create();
        photo->SetString("Type", "Photo");
        direct_play->SetDictionary(dp_idx++, photo);
    }

    // mpv handles both Embed and External natively, so no need to distinguish.
    auto sub_profiles = CefListValue::Create();
    size_t sp_idx = 0;
    for (const auto& fmt : subtitle_names) {
        sub_profiles->SetDictionary(sp_idx++, make_subtitle_profile(fmt, "Embed"));
        sub_profiles->SetDictionary(sp_idx++, make_subtitle_profile(fmt, "External"));
    }

    // TranscodingProfiles: describes what server should produce when a
    // transcode is unavoidable. Order of VideoCodec/AudioCodec is the
    // server's preference order.
    auto transcoding = CefListValue::Create();
    {
        size_t tp_idx = 0;

        auto audio = CefDictionaryValue::Create();
        audio->SetString("Type", "Audio");
        transcoding->SetDictionary(tp_idx++, audio);

        if (!force_transcode) {
            auto fmp4 = CefDictionaryValue::Create();
            fmp4->SetString("Container", "mp4");
            fmp4->SetString("Type", "Video");
            fmp4->SetString("Protocol", "hls");
            fmp4->SetString("AudioCodec", transcode_audio_csv_mp4);
            fmp4->SetString("VideoCodec", transcode_video_csv);
            fmp4->SetString("MaxAudioChannels", kTranscodeMaxAudioChannels);
            transcoding->SetDictionary(tp_idx++, fmp4);
        }

        auto video = CefDictionaryValue::Create();
        video->SetString("Container", kTranscodeContainer);
        video->SetString("Type", "Video");
        video->SetString("Protocol", kTranscodeProtocol);
        video->SetString("AudioCodec", transcode_audio_csv_ts);
        video->SetString("VideoCodec", transcode_video_csv);
        video->SetString("MaxAudioChannels", kTranscodeMaxAudioChannels);
        transcoding->SetDictionary(tp_idx++, video);

        auto photo = CefDictionaryValue::Create();
        photo->SetString("Container", "jpeg");
        photo->SetString("Type", "Photo");
        transcoding->SetDictionary(tp_idx++, photo);
    }
    auto profile = CefDictionaryValue::Create();
    profile->SetString("Name", std::string(device_name));
    profile->SetInt("MaxStaticBitrate", 1000000000);
    profile->SetInt("MusicStreamingTranscodingBitrate", 1280000);
    profile->SetInt("TimelineOffsetSeconds", 5);
    profile->SetList("DirectPlayProfiles", direct_play);
    profile->SetList("TranscodingProfiles", transcoding);
    profile->SetList("SubtitleProfiles", sub_profiles);
    profile->SetList("ResponseProfiles", CefListValue::Create());
    profile->SetList("ContainerProfiles", CefListValue::Create());
    profile->SetList("CodecProfiles", CefListValue::Create());
    (void)app_version;  // reserved for future profile fields if needed

    auto val = CefValue::Create();
    val->SetDictionary(profile);
    std::string json = CefWriteJSON(val, JSON_WRITER_DEFAULT).ToString();
    LOG_INFO(LOG_MAIN, "Device profile: {}", json);
    return json;
}

namespace {
std::string g_cached_json;
}

void SetCachedJson(std::string json) { g_cached_json = std::move(json); }
const std::string& CachedJson() { return g_cached_json; }

}  // namespace jellyfin_device_profile
