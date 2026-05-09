#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "player/mpris/mpris_projection.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

PlaybackSnapshot stoppedSnap() {
    return PlaybackSnapshot{};
}

PlaybackSnapshot playingSnap() {
    PlaybackSnapshot s;
    s.presence = PlayerPresence::Present;
    s.phase = PlaybackPhase::Playing;
    return s;
}

PlaybackSnapshot pausedSnap() {
    PlaybackSnapshot s;
    s.presence = PlayerPresence::Present;
    s.phase = PlaybackPhase::Paused;
    return s;
}

MediaMetadata makeMeta(int64_t duration_us, std::string title = "T") {
    MediaMetadata m;
    m.title = std::move(title);
    m.duration_us = duration_us;
    return m;
}

bool contains(const std::vector<const char*>& v, const char* needle) {
    return std::any_of(v.begin(), v.end(),
        [&](const char* s) { return std::string(s) == needle; });
}

}  // namespace

TEST_CASE("default view is fully Stopped") {
    auto v = project(stoppedSnap(), MprisContent{});
    CHECK(v.playback_status == "Stopped");
    CHECK_FALSE(v.can_play);
    CHECK_FALSE(v.can_pause);
    CHECK_FALSE(v.can_seek);
    CHECK_FALSE(v.can_control);
    CHECK(v.metadata.duration_us == 0);
    CHECK(v.rate == 0.0);
}

TEST_CASE("Stopped -> Playing flips transport caps but not CanSeek without duration") {
    MprisContent c;
    auto a = project(stoppedSnap(), c);
    auto b = project(playingSnap(), c);
    auto d = diff(a, b);
    CHECK(contains(d, "PlaybackStatus"));
    CHECK(contains(d, "CanPlay"));
    CHECK(contains(d, "CanPause"));
    CHECK(contains(d, "CanControl"));
    CHECK_FALSE(contains(d, "CanSeek"));  // duration still 0
    CHECK_FALSE(contains(d, "Metadata")); // both empty
}

TEST_CASE("setMetadata-after-Playing race: duration arriving while Playing flips CanSeek") {
    // Sequence: state goes Playing first (no metadata yet), THEN metadata
    // arrives. The diff between the two computed views must include
    // CanSeek so MPRIS clients refresh their cached value.
    MprisContent c0;
    auto v_after_playing = project(playingSnap(), c0);

    MprisContent c1 = c0;
    c1.metadata = makeMeta(60'000'000);
    auto v_after_meta = project(playingSnap(), c1);

    auto d = diff(v_after_playing, v_after_meta);
    CHECK(contains(d, "Metadata"));
    CHECK(contains(d, "CanSeek"));
    CHECK_FALSE(contains(d, "PlaybackStatus"));  // didn't change
}

TEST_CASE("setMetadata-before-Playing race: same end state, different intermediate") {
    // Reverse order: metadata first (state Stopped), then Playing.
    MprisContent c;
    c.metadata = makeMeta(60'000'000);

    auto v_meta_only = project(stoppedSnap(), c);
    auto d_meta = diff(MprisView{}, v_meta_only);
    CHECK_FALSE(contains(d_meta, "Metadata"));   // suppressed while Stopped
    CHECK_FALSE(contains(d_meta, "CanSeek"));

    auto v_playing = project(playingSnap(), c);
    auto d_play = diff(v_meta_only, v_playing);
    CHECK(contains(d_play, "PlaybackStatus"));
    CHECK(contains(d_play, "Metadata"));
    CHECK(contains(d_play, "CanSeek"));
    CHECK(contains(d_play, "CanPlay"));
    CHECK(contains(d_play, "CanPause"));
    CHECK(contains(d_play, "CanControl"));
}

TEST_CASE("Playing -> Paused: only PlaybackStatus + CanPause flip") {
    MprisContent c;
    c.metadata = makeMeta(10'000'000);
    auto a = project(playingSnap(), c);
    auto b = project(pausedSnap(), c);
    auto d = diff(a, b);
    CHECK(contains(d, "PlaybackStatus"));
    CHECK(contains(d, "CanPause"));
    CHECK_FALSE(contains(d, "CanPlay"));    // still active
    CHECK_FALSE(contains(d, "CanSeek"));    // still seekable
    CHECK_FALSE(contains(d, "CanControl")); // still active
    CHECK_FALSE(contains(d, "Metadata"));
}

TEST_CASE("buffering while phase=Playing keeps status Playing; Rate drops to 0") {
    MprisContent c;
    c.metadata = makeMeta(10'000'000);
    auto playing = playingSnap();
    auto buffering = playing;
    buffering.buffering = true;

    auto v_play = project(playing, c);
    auto v_buf  = project(buffering, c);
    CHECK(v_play.playback_status == "Playing");
    CHECK(v_buf.playback_status  == "Playing");
    CHECK(v_play.rate == 1.0);
    CHECK(v_buf.rate  == 0.0);

    auto d = diff(v_play, v_buf);
    CHECK_FALSE(contains(d, "PlaybackStatus"));
    CHECK(contains(d, "Rate"));
}

TEST_CASE("buffering true locks Rate to 0; false restores pending_rate") {
    MprisContent c;
    c.pending_rate = 1.5;

    auto playing = playingSnap();
    auto v_clear = project(playing, c);
    CHECK(v_clear.rate == 1.5);

    auto buffering = playing;
    buffering.buffering = true;
    auto v_buf = project(buffering, c);
    CHECK(v_buf.rate == 0.0);

    auto d_on  = diff(v_clear, v_buf);
    auto d_off = diff(v_buf, v_clear);
    CHECK(contains(d_on, "Rate"));
    CHECK(contains(d_off, "Rate"));
}

TEST_CASE("seeking while already buffering does not re-emit Rate (already 0)") {
    MprisContent c;
    c.pending_rate = 1.0;

    auto playing = playingSnap();
    auto buffering = playing;
    buffering.buffering = true;
    auto buffering_and_seeking = buffering;
    buffering_and_seeking.seeking = true;

    auto a = project(buffering, c);
    auto b = project(buffering_and_seeking, c);
    CHECK(a.rate == 0.0);
    CHECK(b.rate == 0.0);
    auto d = diff(a, b);
    CHECK_FALSE(contains(d, "Rate"));
}

TEST_CASE("redundant input produces empty diff") {
    MprisContent c;
    c.metadata = makeMeta(60'000'000);
    auto a = project(playingSnap(), c);
    auto b = project(playingSnap(), c);
    CHECK(diff(a, b).empty());
}

TEST_CASE("transition to Stopped clears Metadata + transport caps") {
    MprisContent c;
    c.metadata = makeMeta(10'000'000);
    auto a = project(playingSnap(), c);
    auto b = project(stoppedSnap(), c);
    auto d = diff(a, b);
    CHECK(contains(d, "PlaybackStatus"));
    CHECK(contains(d, "CanPlay"));
    CHECK(contains(d, "CanPause"));
    CHECK(contains(d, "CanSeek"));
    CHECK(contains(d, "CanControl"));
    CHECK(contains(d, "Metadata"));   // suppressed in projection while Stopped
}

TEST_CASE("CanGoNext / CanGoPrevious are independent of playback state") {
    MprisContent c0;
    MprisContent c1 = c0;
    c1.can_go_next = true;
    c1.can_go_previous = true;

    auto a = project(stoppedSnap(), c0);
    auto b = project(stoppedSnap(), c1);
    auto d = diff(a, b);
    CHECK(contains(d, "CanGoNext"));
    CHECK(contains(d, "CanGoPrevious"));
}

TEST_CASE("Volume change isolated to Volume diff") {
    MprisContent c0; c0.volume = 0.5;
    MprisContent c1 = c0; c1.volume = 0.7;
    auto a = project(playingSnap(), c0);
    auto b = project(playingSnap(), c1);
    auto d = diff(a, b);
    REQUIRE(d.size() == 1);
    CHECK(std::string(d[0]) == "Volume");
}

TEST_CASE("Starting phase projects as Playing with Rate=0") {
    PlaybackSnapshot s;
    s.presence = PlayerPresence::Present;
    s.phase = PlaybackPhase::Starting;
    MprisContent c;
    c.metadata = makeMeta(10'000'000);
    auto v = project(s, c);
    CHECK(v.playback_status == "Playing");
    CHECK(v.rate == 0.0);
    CHECK(v.can_play);
    CHECK(v.can_pause);
    CHECK(v.can_seek);
    CHECK(v.can_control);
    CHECK(v.metadata.duration_us == 10'000'000);
}

TEST_CASE("Starting -> Playing flips Rate without churning PlaybackStatus") {
    MprisContent c;
    c.metadata = makeMeta(10'000'000);
    PlaybackSnapshot starting;
    starting.presence = PlayerPresence::Present;
    starting.phase = PlaybackPhase::Starting;
    auto v_pre = project(starting, c);
    auto v_play = project(playingSnap(), c);
    auto d = diff(v_pre, v_play);
    CHECK(contains(d, "Rate"));
    CHECK_FALSE(contains(d, "PlaybackStatus"));
    CHECK_FALSE(contains(d, "CanPause"));
}

TEST_CASE("track switch: Starting + new metadata vs prev Playing track") {
    MprisContent prev;
    prev.metadata = makeMeta(10'000'000, "A");
    auto v_a_playing = project(playingSnap(), prev);

    MprisContent next;
    next.metadata = makeMeta(20'000'000, "B");
    PlaybackSnapshot starting;
    starting.presence = PlayerPresence::Present;
    starting.phase = PlaybackPhase::Starting;
    auto v_b_starting = project(starting, next);

    auto d = diff(v_a_playing, v_b_starting);
    CHECK(contains(d, "Rate"));              // 1.0 -> 0.0
    CHECK(contains(d, "Metadata"));          // A -> B
    CHECK_FALSE(contains(d, "PlaybackStatus")); // Playing in both
    CHECK_FALSE(contains(d, "CanPause"));    // active in both
    CHECK_FALSE(contains(d, "CanSeek"));     // both seekable
    CHECK_FALSE(contains(d, "CanPlay"));
    CHECK_FALSE(contains(d, "CanControl"));
}
