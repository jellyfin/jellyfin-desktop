#pragma once

#include "playback_event.h"
#include "playback_state_machine.h"
#include "../wake_event.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Thread-safe coordinator that owns the only mutable PlaybackStateMachine.
// Producers (mpv adapter, browser IPC) post inputs; coordinator drains
// them on its worker thread, runs transitions, updates the canonical
// snapshot, and tryPosts emitted events to all registered sinks.
//
// Sinks must be added before start(). The coordinator never invokes
// observers inline on producer threads — sinks own their executors.
class PlaybackCoordinator {
public:
    PlaybackCoordinator();
    ~PlaybackCoordinator();

    PlaybackCoordinator(const PlaybackCoordinator&) = delete;
    PlaybackCoordinator& operator=(const PlaybackCoordinator&) = delete;

    void addSink(std::shared_ptr<PlaybackEventSink> sink);
    void addActionSink(std::shared_ptr<PlaybackActionSink> sink);

    void start();
    void stop();

    // Inputs — safe from any thread.
    void postFileLoaded();
    void postLoadStarting(std::string item_id = {});
    void postPauseChanged(bool paused);
    void postEndFile(EndReason reason, std::string error_message = {});
    void postSeekingChanged(bool seeking);
    void postPausedForCache(bool paused_for_cache);
    void postCoreIdle(bool core_idle);
    void postPosition(int64_t position_us);
    void postMediaType(MediaType type);
    void postVideoFrameAvailable(bool available);
    void postSpeed(double rate);
    void postDuration(int64_t duration_us);
    void postFullscreen(bool fullscreen, bool was_maximized);
    void postOsdDims(int lw, int lh, int pw, int ph);
    void postBufferedRanges(std::vector<PlaybackBufferedRange> ranges);
    void postDisplayHz(int hz);

    // Canonical snapshot. Read-only consumers (hotkeys, idle inhibit)
    // call this instead of touching the SM directly.
    PlaybackSnapshot snapshot() const;

private:
    struct Input {
        enum class Kind {
            FileLoaded, LoadStarting, PauseChanged, EndFile,
            SeekingChanged, PausedForCache, CoreIdle, Position, MediaType,
            VideoFrameAvailable, Speed, Duration, Fullscreen, OsdDims,
            BufferedRanges, DisplayHz,
        };
        Kind kind;
        bool flag = false;
        bool flag2 = false;     // Fullscreen: was_maximized
        int64_t i64 = 0;
        double dbl = 0.0;       // Speed
        int lw = 0, lh = 0;     // OsdDims
        int pw = 0, ph = 0;     // OsdDims
        int hz = 0;             // DisplayHz
        EndReason reason = EndReason::Eof;
        ::MediaType media_type = ::MediaType::Unknown;
        std::string str;
        std::vector<PlaybackBufferedRange> ranges;
    };

    void enqueue(Input in);
    void worker();
    void apply(const Input& in, std::vector<PlaybackEvent>& out);

    std::vector<std::shared_ptr<PlaybackEventSink>> sinks_;
    std::vector<std::shared_ptr<PlaybackActionSink>> action_sinks_;

    std::mutex queue_mutex_;
    std::deque<Input> queue_;
    WakeEvent wake_;

    mutable std::mutex snapshot_mutex_;
    PlaybackSnapshot snapshot_;

    PlaybackStateMachine sm_;  // worker-thread-only
    std::thread thread_;
    std::atomic<bool> running_{false};
};

// RAII wrapper used by run_with_cef to keep the coordinator's lifetime
// bracketed by main(). Construction starts the worker; destruction stops
// it. Sinks must be added between construction and the first input post.
class PlaybackCoordinatorScope {
public:
    PlaybackCoordinatorScope();
    ~PlaybackCoordinatorScope();
    PlaybackCoordinator& coord() { return coord_; }
private:
    PlaybackCoordinator coord_;
};
