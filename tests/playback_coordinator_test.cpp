#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "player/playback_coordinator.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

class FakeSink final : public PlaybackEventSink {
public:
    bool tryPost(const PlaybackEvent& ev) override {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(ev);
        return true;
    }
    std::vector<PlaybackEvent> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto out = events_;
        events_.clear();
        return out;
    }
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_.size();
    }
private:
    std::mutex mutex_;
    std::vector<PlaybackEvent> events_;
};

void wait_until(std::function<bool()> pred,
                std::chrono::milliseconds budget = std::chrono::milliseconds(1000)) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // namespace

TEST_CASE("coordinator delivers events in order across sinks") {
    PlaybackCoordinator coord;
    auto sink_a = std::make_shared<FakeSink>();
    auto sink_b = std::make_shared<FakeSink>();
    coord.addSink(sink_a);
    coord.addSink(sink_b);
    coord.start();

    coord.postFileLoaded();
    coord.postPauseChanged(false);
    coord.postPauseChanged(true);

    wait_until([&] { return sink_a->size() >= 3 && sink_b->size() >= 3; });

    auto a = sink_a->drain();
    auto b = sink_b->drain();
    REQUIRE(a.size() >= 3);
    REQUIRE(b.size() >= 3);
    CHECK(a[0].kind == PlaybackEvent::Kind::TrackLoaded);
    CHECK(a[1].kind == PlaybackEvent::Kind::Started);
    CHECK(a[2].kind == PlaybackEvent::Kind::Paused);
    CHECK(b[0].kind == PlaybackEvent::Kind::TrackLoaded);
    CHECK(b[1].kind == PlaybackEvent::Kind::Started);
    CHECK(b[2].kind == PlaybackEvent::Kind::Paused);

    coord.stop();
}

TEST_CASE("coordinator snapshot reflects post-transition state") {
    PlaybackCoordinator coord;
    auto sink = std::make_shared<FakeSink>();
    coord.addSink(sink);
    coord.start();

    coord.postMediaType(MediaType::Video);
    coord.postFileLoaded();
    coord.postPauseChanged(false);
    coord.postVideoFrameAvailable(true);

    wait_until([&] {
        auto s = coord.snapshot();
        return s.phase == PlaybackPhase::Playing
            && s.media_type == MediaType::Video;
    });
    auto s = coord.snapshot();
    CHECK(s.phase == PlaybackPhase::Playing);
    CHECK(s.presence == PlayerPresence::Present);
    CHECK(s.media_type == MediaType::Video);

    coord.stop();
}

TEST_CASE("coordinator clears state on terminal event") {
    PlaybackCoordinator coord;
    auto sink = std::make_shared<FakeSink>();
    coord.addSink(sink);
    coord.start();

    coord.postFileLoaded();
    coord.postPauseChanged(false);
    coord.postSeekingChanged(true);
    coord.postEndFile(EndReason::Eof);

    wait_until([&] {
        auto s = coord.snapshot();
        return s.phase == PlaybackPhase::Stopped
            && s.presence == PlayerPresence::None
            && s.seeking == false;
    });
    auto s = coord.snapshot();
    CHECK(s.phase == PlaybackPhase::Stopped);
    CHECK(s.presence == PlayerPresence::None);
    CHECK(s.seeking == false);

    coord.stop();
}

TEST_CASE("stop with no inputs is a clean no-op") {
    PlaybackCoordinator coord;
    coord.start();
    coord.stop();
    auto s = coord.snapshot();
    CHECK(s.phase == PlaybackPhase::Stopped);
}
