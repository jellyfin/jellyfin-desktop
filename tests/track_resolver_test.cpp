#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "player/track_resolver.h"

using track_resolver::kTrackAuto;
using track_resolver::kTrackDisable;
using track_resolver::MediaStream;
using track_resolver::resolve_audio_aid;
using track_resolver::resolve_subtitle;
using track_resolver::SubtitleSelection;
using track_resolver::SubtitleUnresolvedFallback;

namespace {

MediaStream video(int idx) { return {idx, "Video", false, "", ""}; }
MediaStream audio(int idx) { return {idx, "Audio", false, "", ""}; }
MediaStream sub_embedded(int idx) { return {idx, "Subtitle", false, "Embed", ""}; }
MediaStream sub_external(int idx, std::string url) {
    return {idx, "Subtitle", false, "External", std::move(url)};
}
MediaStream sub_external_marked(int idx, std::string url) {
    // Same as sub_external but with IsExternal=true to confirm both flags
    // independently route to the External path.
    return {idx, "Subtitle", true, "External", std::move(url)};
}

}  // namespace

// =====================================================================
// Audio
// =====================================================================

TEST_CASE("audio: missing/null index → kTrackAuto") {
    std::vector<MediaStream> streams{video(0), audio(1)};
    CHECK(resolve_audio_aid(streams, std::nullopt) == kTrackAuto);
}

TEST_CASE("audio: negative index → kTrackAuto") {
    std::vector<MediaStream> streams{video(0), audio(1)};
    CHECK(resolve_audio_aid(streams, -1) == kTrackAuto);
}

TEST_CASE("audio: first audio stream → 1") {
    std::vector<MediaStream> streams{video(0), audio(1), audio(2), audio(3)};
    CHECK(resolve_audio_aid(streams, 1) == 1);
}

TEST_CASE("audio: third audio stream → 3") {
    std::vector<MediaStream> streams{video(0), audio(1), audio(2), audio(3)};
    CHECK(resolve_audio_aid(streams, 3) == 3);
}

TEST_CASE("audio: index of non-audio stream → kTrackAuto") {
    std::vector<MediaStream> streams{video(0), audio(1)};
    CHECK(resolve_audio_aid(streams, 0) == kTrackAuto);  // points at video
}

TEST_CASE("audio: index that doesn't exist → kTrackAuto") {
    std::vector<MediaStream> streams{video(0), audio(1)};
    CHECK(resolve_audio_aid(streams, 99) == kTrackAuto);
}

TEST_CASE("audio: external streams are skipped during relative walk") {
    std::vector<MediaStream> streams{
        video(0),
        audio(1),
        {2, "Audio", true, "", ""},  // external audio (rare but possible)
        audio(3),
    };
    // Stream 3 should resolve to relative index 2 because the external (2) is skipped.
    CHECK(resolve_audio_aid(streams, 3) == 2);
}

TEST_CASE("audio: empty streams → kTrackAuto") {
    std::vector<MediaStream> streams;
    CHECK(resolve_audio_aid(streams, 0) == kTrackAuto);
    CHECK(resolve_audio_aid(streams, std::nullopt) == kTrackAuto);
}

// =====================================================================
// Subtitle (load fallback = AutoFallback)
// =====================================================================

TEST_CASE("subtitle load: missing/null → Disable") {
    std::vector<MediaStream> streams{sub_embedded(1)};
    auto r = resolve_subtitle(streams, std::nullopt,
                              SubtitleUnresolvedFallback::AutoFallback);
    CHECK(r.kind == SubtitleSelection::Kind::Disable);
    CHECK(r.sid == kTrackDisable);
    CHECK(r.url == "");
}

TEST_CASE("subtitle load: negative → Disable") {
    std::vector<MediaStream> streams{sub_embedded(1)};
    auto r = resolve_subtitle(streams, -1,
                              SubtitleUnresolvedFallback::AutoFallback);
    CHECK(r.kind == SubtitleSelection::Kind::Disable);
}

TEST_CASE("subtitle load: external by DeliveryMethod → External + URL") {
    std::vector<MediaStream> streams{
        video(0),
        sub_external(1, "http://srv/en.vtt"),
    };
    auto r = resolve_subtitle(streams, 1,
                              SubtitleUnresolvedFallback::AutoFallback);
    CHECK(r.kind == SubtitleSelection::Kind::External);
    CHECK(r.url == "http://srv/en.vtt");
}

TEST_CASE("subtitle load: External with empty URL falls through to relative path") {
    std::vector<MediaStream> streams{
        video(0),
        {1, "Subtitle", false, "External", ""},  // no URL
    };
    auto r = resolve_subtitle(streams, 1,
                              SubtitleUnresolvedFallback::AutoFallback);
    // Falls through to relative index walk; this is still a Subtitle so it
    // resolves to embedded sid=1.
    CHECK(r.kind == SubtitleSelection::Kind::Embedded);
    CHECK(r.sid == 1);
}

TEST_CASE("subtitle load: embedded → Embedded + relative sid") {
    std::vector<MediaStream> streams{
        video(0),
        audio(1),
        sub_embedded(2),
        sub_embedded(3),
    };
    auto r = resolve_subtitle(streams, 3,
                              SubtitleUnresolvedFallback::AutoFallback);
    CHECK(r.kind == SubtitleSelection::Kind::Embedded);
    CHECK(r.sid == 2);
}

TEST_CASE("subtitle load: index of non-subtitle stream → AutoFallback Embedded sid=kTrackAuto") {
    std::vector<MediaStream> streams{video(0), audio(1)};
    auto r = resolve_subtitle(streams, 1,  // points at audio
                              SubtitleUnresolvedFallback::AutoFallback);
    CHECK(r.kind == SubtitleSelection::Kind::Embedded);
    CHECK(r.sid == kTrackAuto);
}

TEST_CASE("subtitle load: index that doesn't exist → AutoFallback Embedded sid=kTrackAuto") {
    std::vector<MediaStream> streams{sub_embedded(1)};
    auto r = resolve_subtitle(streams, 99,
                              SubtitleUnresolvedFallback::AutoFallback);
    CHECK(r.kind == SubtitleSelection::Kind::Embedded);
    CHECK(r.sid == kTrackAuto);
}

// =====================================================================
// Subtitle (runtime change fallback = DisableFallback)
// =====================================================================

TEST_CASE("subtitle change: missing/null → Disable") {
    std::vector<MediaStream> streams{sub_embedded(1)};
    auto r = resolve_subtitle(streams, std::nullopt,
                              SubtitleUnresolvedFallback::DisableFallback);
    CHECK(r.kind == SubtitleSelection::Kind::Disable);
}

TEST_CASE("subtitle change: external → External + URL (regardless of fallback)") {
    std::vector<MediaStream> streams{sub_external(4, "http://srv/x.vtt")};
    auto r = resolve_subtitle(streams, 4,
                              SubtitleUnresolvedFallback::DisableFallback);
    CHECK(r.kind == SubtitleSelection::Kind::External);
    CHECK(r.url == "http://srv/x.vtt");
}

TEST_CASE("subtitle change: index doesn't exist → Disable (NOT auto)") {
    std::vector<MediaStream> streams{sub_embedded(1)};
    auto r = resolve_subtitle(streams, 99,
                              SubtitleUnresolvedFallback::DisableFallback);
    CHECK(r.kind == SubtitleSelection::Kind::Disable);
    CHECK(r.sid == kTrackDisable);
}

TEST_CASE("subtitle change: embedded resolves the same in both modes") {
    std::vector<MediaStream> streams{
        video(0), audio(1), sub_embedded(2), sub_embedded(3),
    };
    auto a = resolve_subtitle(streams, 3, SubtitleUnresolvedFallback::AutoFallback);
    auto b = resolve_subtitle(streams, 3, SubtitleUnresolvedFallback::DisableFallback);
    CHECK(a.kind == SubtitleSelection::Kind::Embedded);
    CHECK(b.kind == SubtitleSelection::Kind::Embedded);
    CHECK(a.sid == 2);
    CHECK(b.sid == 2);
}

TEST_CASE("subtitle: IsExternal=true streams are skipped during relative walk") {
    std::vector<MediaStream> streams{
        video(0),
        sub_embedded(1),
        sub_external_marked(2, "http://srv/ext.vtt"),  // IsExternal=true, External method
        sub_embedded(3),
    };
    // Stream 3 should be relative index 2 (external 2 skipped).
    auto r = resolve_subtitle(streams, 3,
                              SubtitleUnresolvedFallback::AutoFallback);
    CHECK(r.kind == SubtitleSelection::Kind::Embedded);
    CHECK(r.sid == 2);

    // Stream 2 itself goes through External path because of DeliveryMethod.
    auto r2 = resolve_subtitle(streams, 2,
                               SubtitleUnresolvedFallback::AutoFallback);
    CHECK(r2.kind == SubtitleSelection::Kind::External);
    CHECK(r2.url == "http://srv/ext.vtt");
}
