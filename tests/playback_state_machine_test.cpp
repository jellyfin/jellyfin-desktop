#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "player/playback_state_machine.h"

namespace {

bool has(const std::vector<PlaybackEvent>& v, PlaybackEvent::Kind k) {
    for (const auto& e : v) if (e.kind == k) return true;
    return false;
}

}  // namespace

TEST_CASE("default snapshot is stopped + absent") {
    PlaybackStateMachine sm;
    auto s = sm.snapshot();
    CHECK(s.presence == PlayerPresence::None);
    CHECK(s.phase == PlaybackPhase::Stopped);
    CHECK(s.seeking == false);
    CHECK(s.buffering == false);
    CHECK(s.media_type == MediaType::Unknown);
    CHECK(s.position_us == 0);
}

TEST_CASE("file loaded enters Present + Starting and emits TrackLoaded") {
    PlaybackStateMachine sm;
    auto out = sm.onFileLoaded();
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == PlaybackEvent::Kind::TrackLoaded);
    auto s = sm.snapshot();
    CHECK(s.presence == PlayerPresence::Present);
    CHECK(s.phase == PlaybackPhase::Starting);
}

TEST_CASE("onLoadStarting transitions to Present + Starting and emits TrackLoaded") {
    PlaybackStateMachine sm;
    auto out = sm.onLoadStarting();
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == PlaybackEvent::Kind::TrackLoaded);
    auto s = sm.snapshot();
    CHECK(s.presence == PlayerPresence::Present);
    CHECK(s.phase == PlaybackPhase::Starting);
}

TEST_CASE("position seeded before FILE_LOADED survives the loadfile round-trip") {
    PlaybackStateMachine sm;
    sm.onLoadStarting();
    sm.onPosition(5'000'000);  // JS-driven resume seed
    CHECK(sm.snapshot().position_us == 5'000'000);
    sm.onFileLoaded();
    CHECK(sm.snapshot().position_us == 5'000'000);
}

TEST_CASE("track switch preserves seeded position across swallowed EOF") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    sm.onPosition(1'000'000);

    sm.onLoadStarting();
    sm.onPosition(8'000'000);  // seed for next track
    auto out = sm.onEndFile(EndReason::Eof);
    CHECK_FALSE(has(out, PlaybackEvent::Kind::Finished));
    CHECK(sm.snapshot().position_us == 8'000'000);
    sm.onFileLoaded();
    CHECK(sm.snapshot().position_us == 8'000'000);
}

TEST_CASE("variant_switch_pending: false on first load, true on same-Id reload, spans through FILE_LOADED until Started") {
    PlaybackStateMachine sm;
    sm.onLoadStarting("item-A");
    CHECK(sm.snapshot().variant_switch_pending == false);
    sm.onFileLoaded();
    CHECK(sm.snapshot().variant_switch_pending == false);
    sm.onPauseChanged(false);
    CHECK(sm.snapshot().phase == PlaybackPhase::Playing);

    // Variant switch begins.
    sm.onLoadStarting("item-A");
    CHECK(sm.snapshot().variant_switch_pending == true);
    // FILE_LOADED for the new variant must NOT clear the flag.
    sm.onFileLoaded();
    CHECK(sm.snapshot().variant_switch_pending == true);
    // First-frame Started clears it.
    sm.onPauseChanged(false);
    CHECK(sm.snapshot().phase == PlaybackPhase::Playing);
    CHECK(sm.snapshot().variant_switch_pending == false);
}

TEST_CASE("variant_switch_pending cleared by Started from buffering-clear path") {
    PlaybackStateMachine sm;
    sm.onLoadStarting("item-A");
    sm.onFileLoaded();
    sm.onPauseChanged(false);  // initial Started

    sm.onLoadStarting("item-A");
    sm.onFileLoaded();
    CHECK(sm.snapshot().variant_switch_pending == true);
    sm.onPausedForCache(true);
    sm.onPauseChanged(false);  // gated by buffering, no Started yet
    CHECK(sm.snapshot().variant_switch_pending == true);
    auto out = sm.onPausedForCache(false);
    CHECK(has(out, PlaybackEvent::Kind::Started));
    CHECK(sm.snapshot().variant_switch_pending == false);
}

TEST_CASE("variant_switch_pending cleared by terminal end-file") {
    PlaybackStateMachine sm;
    sm.onLoadStarting("item-A");
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    sm.onLoadStarting("item-A");
    CHECK(sm.snapshot().variant_switch_pending == true);
    sm.onEndFile(EndReason::Error, "boom");
    CHECK(sm.snapshot().variant_switch_pending == false);
}

TEST_CASE("variant_switch_pending: false on different-Id reload") {
    PlaybackStateMachine sm;
    sm.onLoadStarting("item-A");
    sm.onFileLoaded();
    sm.onLoadStarting("item-B");
    CHECK(sm.snapshot().variant_switch_pending == false);
}

TEST_CASE("variant_switch_pending: empty Id never marks variant") {
    PlaybackStateMachine sm;
    sm.onLoadStarting("");
    sm.onFileLoaded();
    sm.onLoadStarting("");
    CHECK(sm.snapshot().variant_switch_pending == false);
}

TEST_CASE("variant_switch_pending: terminal end-file clears identity") {
    PlaybackStateMachine sm;
    sm.onLoadStarting("item-A");
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    sm.onEndFile(EndReason::Eof);
    // After terminal, even a same-Id load is treated as fresh.
    sm.onLoadStarting("item-A");
    CHECK(sm.snapshot().variant_switch_pending == false);
}

TEST_CASE("track switch: pending_load swallows EOF and stays Present + Starting") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    REQUIRE(sm.snapshot().phase == PlaybackPhase::Playing);

    sm.onLoadStarting();
    auto out = sm.onEndFile(EndReason::Eof);
    CHECK(has(out, PlaybackEvent::Kind::Finished) == false);
    CHECK(has(out, PlaybackEvent::Kind::Canceled) == false);
    auto s = sm.snapshot();
    CHECK(s.presence == PlayerPresence::Present);
    CHECK(s.phase == PlaybackPhase::Starting);

    // Followed by FILE_LOADED + pause flip for new track.
    auto loaded = sm.onFileLoaded();
    CHECK(has(loaded, PlaybackEvent::Kind::TrackLoaded));
    auto resumed = sm.onPauseChanged(false);
    CHECK(has(resumed, PlaybackEvent::Kind::Started));
}

TEST_CASE("track switch: pending_load also swallows REASON_STOP cancel") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    sm.onLoadStarting();
    auto out = sm.onEndFile(EndReason::Canceled);
    CHECK_FALSE(has(out, PlaybackEvent::Kind::Canceled));
    CHECK(sm.snapshot().phase == PlaybackPhase::Starting);
}

TEST_CASE("pending_load does NOT swallow Error end-files") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    sm.onLoadStarting();
    auto out = sm.onEndFile(EndReason::Error, "boom");
    CHECK(has(out, PlaybackEvent::Kind::Error));
    CHECK(sm.snapshot().phase == PlaybackPhase::Stopped);
    CHECK(sm.snapshot().presence == PlayerPresence::None);
}

TEST_CASE("FILE_LOADED clears any stale pending_load") {
    PlaybackStateMachine sm;
    sm.onLoadStarting();
    sm.onFileLoaded();
    // Now an EOF without further onLoadStarting should be terminal.
    auto out = sm.onEndFile(EndReason::Eof);
    CHECK(has(out, PlaybackEvent::Kind::Finished));
}

TEST_CASE("pause events while idle/stopped are ignored") {
    PlaybackStateMachine sm;
    CHECK(sm.onPauseChanged(false).empty());
    CHECK(sm.onPauseChanged(true).empty());
    CHECK(sm.snapshot().phase == PlaybackPhase::Stopped);
    CHECK(sm.snapshot().presence == PlayerPresence::None);
}

TEST_CASE("Started waits for mpv core-idle=false after pause=false") {
    // Pre-roll: mpv reports core-idle=true (initial value via property
    // observation, before any file load). FILE_LOADED then enters
    // Starting with buffering=true; pause=false alone must NOT promote
    // to Playing — Started waits for mpv's first-frame edge.
    PlaybackStateMachine sm;
    sm.onCoreIdle(true);
    sm.onFileLoaded();
    CHECK(sm.snapshot().buffering == true);

    auto unpause = sm.onPauseChanged(false);
    CHECK_FALSE(has(unpause, PlaybackEvent::Kind::Started));
    CHECK(sm.snapshot().phase == PlaybackPhase::Starting);

    auto cleared = sm.onCoreIdle(false);
    CHECK(has(cleared, PlaybackEvent::Kind::Started));
    CHECK(sm.snapshot().phase == PlaybackPhase::Playing);
    CHECK(sm.snapshot().buffering == false);
}

TEST_CASE("core-idle observed while idle is preserved across FILE_LOADED") {
    PlaybackStateMachine sm;
    auto idle_set = sm.onCoreIdle(true);
    CHECK(idle_set.empty());
    auto loaded = sm.onFileLoaded();
    CHECK(has(loaded, PlaybackEvent::Kind::TrackLoaded));
    CHECK(sm.snapshot().buffering == true);
}

TEST_CASE("paused-for-cache observed while idle is preserved across FILE_LOADED") {
    PlaybackStateMachine sm;
    auto pfc_set = sm.onPausedForCache(true);
    CHECK(pfc_set.empty());
    auto loaded = sm.onFileLoaded();
    CHECK(has(loaded, PlaybackEvent::Kind::TrackLoaded));
    CHECK(sm.snapshot().buffering == true);
}

TEST_CASE("pause toggles emit Paused/Started without self-edges") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    auto pause1 = sm.onPauseChanged(true);
    CHECK(has(pause1, PlaybackEvent::Kind::Paused));
    auto pause2 = sm.onPauseChanged(true);
    CHECK(pause2.empty());
    auto resume = sm.onPauseChanged(false);
    CHECK(has(resume, PlaybackEvent::Kind::Started));
    auto resume2 = sm.onPauseChanged(false);
    CHECK(resume2.empty());
}

TEST_CASE("EOF emits Finished and force-clears seeking/buffering") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    sm.onSeekingChanged(true);
    sm.onPausedForCache(true);
    auto out = sm.onEndFile(EndReason::Eof);
    CHECK(has(out, PlaybackEvent::Kind::Finished));
    bool sawSeekingFalse = false, sawBufferingFalse = false;
    for (const auto& e : out) {
        if (e.kind == PlaybackEvent::Kind::SeekingChanged && e.flag == false)
            sawSeekingFalse = true;
        if (e.kind == PlaybackEvent::Kind::BufferingChanged && e.flag == false)
            sawBufferingFalse = true;
    }
    CHECK(sawSeekingFalse);
    CHECK(sawBufferingFalse);
    auto s = sm.snapshot();
    CHECK(s.phase == PlaybackPhase::Stopped);
    CHECK(s.presence == PlayerPresence::None);
    CHECK(s.seeking == false);
    CHECK(s.buffering == false);
}

TEST_CASE("error end-file carries message") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    auto out = sm.onEndFile(EndReason::Error, "boom");
    bool found = false;
    for (const auto& e : out)
        if (e.kind == PlaybackEvent::Kind::Error && e.error_message == "boom")
            found = true;
    CHECK(found);
}

TEST_CASE("cancel end-file emits Canceled") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    auto out = sm.onEndFile(EndReason::Canceled);
    CHECK(has(out, PlaybackEvent::Kind::Canceled));
}

TEST_CASE("seeking changes are edge-triggered") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    auto a = sm.onSeekingChanged(true);
    CHECK(a.size() == 1);
    CHECK(a[0].flag == true);
    auto b = sm.onSeekingChanged(true);
    CHECK(b.empty());
}

TEST_CASE("buffering during Starting holds back Started until buffer clears") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPausedForCache(true);
    auto unpause = sm.onPauseChanged(false);
    CHECK_FALSE(has(unpause, PlaybackEvent::Kind::Started));
    CHECK(sm.snapshot().phase == PlaybackPhase::Starting);

    auto buffer_cleared = sm.onPausedForCache(false);
    CHECK(has(buffer_cleared, PlaybackEvent::Kind::Started));
    CHECK(sm.snapshot().phase == PlaybackPhase::Playing);
}

TEST_CASE("resume from Paused is immediate, not gated on buffering") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);          // Started
    sm.onPauseChanged(true);           // Paused
    sm.onPausedForCache(true);       // tail-end buffering
    auto out = sm.onPauseChanged(false);
    CHECK(has(out, PlaybackEvent::Kind::Started));
    CHECK(sm.snapshot().phase == PlaybackPhase::Playing);
}

TEST_CASE("core-idle alone gates Started during pre-roll") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onCoreIdle(true);                 // VO/decoder still warming up
    auto unpause = sm.onPauseChanged(false);
    CHECK_FALSE(has(unpause, PlaybackEvent::Kind::Started));
    CHECK(sm.snapshot().phase == PlaybackPhase::Starting);
    CHECK(sm.snapshot().buffering == true);

    auto cleared = sm.onCoreIdle(false);
    CHECK(has(cleared, PlaybackEvent::Kind::Started));
    CHECK(sm.snapshot().phase == PlaybackPhase::Playing);
    CHECK(sm.snapshot().buffering == false);
}

TEST_CASE("snapshot.buffering OR's paused-for-cache and core-idle") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    sm.onPausedForCache(true);
    sm.onCoreIdle(true);
    CHECK(sm.snapshot().buffering == true);

    sm.onPausedForCache(false);
    CHECK(sm.snapshot().buffering == true);  // still core-idle

    sm.onCoreIdle(false);
    CHECK(sm.snapshot().buffering == false);
}

TEST_CASE("buffering uses paused-for-cache as input") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    auto a = sm.onPausedForCache(true);
    CHECK(a.size() == 1);
    CHECK(a[0].flag == true);
    auto b = sm.onPausedForCache(false);
    CHECK(b.size() == 1);
    CHECK(b[0].flag == false);
}

TEST_CASE("position update completes seek") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    sm.onSeekingChanged(true);
    auto out = sm.onPosition(1234567);
    CHECK(sm.snapshot().position_us == 1234567);
    CHECK(sm.snapshot().seeking == false);
    bool found = false;
    for (const auto& e : out)
        if (e.kind == PlaybackEvent::Kind::SeekingChanged && e.flag == false)
            found = true;
    CHECK(found);
}

TEST_CASE("media type changes are edge-triggered") {
    PlaybackStateMachine sm;
    auto a = sm.onMediaType(MediaType::Video);
    CHECK(a.size() == 1);
    CHECK(a[0].kind == PlaybackEvent::Kind::MediaTypeChanged);
    CHECK(a[0].media_type == MediaType::Video);
    auto b = sm.onMediaType(MediaType::Video);
    CHECK(b.empty());
    auto c = sm.onMediaType(MediaType::Audio);
    CHECK(c.size() == 1);
}
