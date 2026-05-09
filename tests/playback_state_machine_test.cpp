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

TEST_CASE("file loaded enters Present + Starting without emitting Started") {
    PlaybackStateMachine sm;
    auto out = sm.onFileLoaded();
    CHECK(out.empty());
    auto s = sm.snapshot();
    CHECK(s.presence == PlayerPresence::Present);
    CHECK(s.phase == PlaybackPhase::Starting);
}

TEST_CASE("pause events while idle/stopped are ignored") {
    PlaybackStateMachine sm;
    CHECK(sm.onPauseChanged(false).empty());
    CHECK(sm.onPauseChanged(true).empty());
    CHECK(sm.snapshot().phase == PlaybackPhase::Stopped);
    CHECK(sm.snapshot().presence == PlayerPresence::None);
}

TEST_CASE("first valid pause=false after file load emits Started") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    auto out = sm.onPauseChanged(false);
    CHECK(out.size() == 1);
    CHECK(out[0].kind == PlaybackEvent::Kind::Started);
    CHECK(sm.snapshot().phase == PlaybackPhase::Playing);
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
    sm.onBufferingChanged(true);
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

TEST_CASE("buffering uses paused-for-cache as input") {
    PlaybackStateMachine sm;
    sm.onFileLoaded();
    sm.onPauseChanged(false);
    auto a = sm.onBufferingChanged(true);
    CHECK(a.size() == 1);
    CHECK(a[0].flag == true);
    auto b = sm.onBufferingChanged(false);
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
