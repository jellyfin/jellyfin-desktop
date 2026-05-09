#pragma once

#include "event.h"

#include <cstdint>
#include <string>
#include <vector>

// Pure deterministic state machine. No threads, globals, platform calls,
// CEF calls, media-session calls, or logging. All state lives in the
// returned snapshot. Inputs return the list of semantic events emitted
// by the transition (often empty). Side-effecting transitions also push
// PlaybackActions onto an internal queue, drained via consumeActions().
class PlaybackStateMachine {
public:
    PlaybackSnapshot snapshot() const { return s_; }

    // mpv MPV_EVENT_FILE_LOADED. Enters Present + Starting. Forces
    // seeking off, recomputes buffering. Clears any pending_load. Does
    // NOT reset position_us so any seed established by onPosition before
    // the loadfile round-trip survives; mpv's first time-pos overwrites
    // it once frames roll. Emits TrackLoaded so sinks pick up the new
    // track without waiting for the first PAUSE flip — Started/Paused
    // still wait for that flip. Also emits an
    // ApplyPendingTrackSelectionAndPlay action: mpv loads paused, and
    // the SM owns the "now apply pending track picks then unpause"
    // side effect that used to live in event_dispatcher.
    std::vector<PlaybackEvent> onFileLoaded();

    // Browser-driven hint that a `loadfile` is about to issue. Carries
    // the Jellyfin item Id of the upcoming track so the SM can
    // distinguish a same-item reload (bitrate / transcode-variant
    // change) from a fresh track change. Transitions to Present +
    // Starting and emits TrackLoaded so sinks recompute right away —
    // JS UI and MPRIS reflect the new track before mpv has opened the
    // file. Sets pending_load so the next END_FILE_EOF/CANCEL is
    // treated as a track-switch boundary (no terminal event). Errors
    // still surface as terminal. Position is seeded separately via
    // onPosition. When item_id is non-empty and matches the previously
    // observed Id, sets snapshot.variant_switch_pending=true.
    std::vector<PlaybackEvent> onLoadStarting(std::string item_id = {});

    // mpv 'pause' property change. Pause events while presence==None or
    // phase==Stopped are silently ignored — mpv's pause flag is
    // observable while idle and pause=false there does not mean playback.
    std::vector<PlaybackEvent> onPauseChanged(bool paused);

    // mpv MPV_EVENT_END_FILE. Force-clears seeking/buffering, transitions
    // to None + Stopped, emits a single terminal event matching reason.
    std::vector<PlaybackEvent> onEndFile(EndReason reason,
                                         std::string error_message = {});

    // mpv 'seeking' property change. Self-edges silent.
    std::vector<PlaybackEvent> onSeekingChanged(bool seeking);

    // mpv 'paused-for-cache' property change. Combined with core-idle
    // into the snapshot's `buffering` flag (true if either is true).
    std::vector<PlaybackEvent> onPausedForCache(bool paused_for_cache);

    // mpv 'core-idle' property change. True while playback core isn't
    // actually advancing (VO init, seeks, decode lead-in, etc.) — the
    // strongest "not actually playing" signal mpv exposes.
    std::vector<PlaybackEvent> onCoreIdle(bool core_idle);

    // mpv 'time-pos' update. Updates snapshot position; if seeking is
    // active, the first position update completes the seek (clears the
    // flag and emits SeekingChanged(false)). Always emits PositionChanged
    // when the value actually changed — sinks (browser, media session)
    // need every tick.
    std::vector<PlaybackEvent> onPosition(int64_t position_us);

    // Browser-driven media-type change (Audio/Video/Unknown).
    std::vector<PlaybackEvent> onMediaType(MediaType type);

    // mpv `video-frame-info` property change. `available` reflects whether
    // mpv's VO has a current frame (vo_get_current_frame() non-NULL). The
    // first AVAILABLE after FILE_LOADED is mpv's truthful "first frame on
    // screen" edge — independent of the `core-idle` / PLAYBACK_RESTART
    // path which can fire prematurely while audio/video status are at
    // STATUS_EOF default.
    std::vector<PlaybackEvent> onVideoFrameAvailable(bool available);

    // mpv 'speed' property change. Self-edges silent. Snapshot.rate
    // tracks the live value.
    std::vector<PlaybackEvent> onSpeed(double rate);

    // mpv 'duration' property change. Stored in microseconds. Self-edges
    // silent.
    std::vector<PlaybackEvent> onDuration(int64_t duration_us);

    // mpv 'fullscreen' property change. `was_maximized` captures
    // mpv::window_maximized() at the dispatcher boundary so the SM
    // does not need to call platform code; it lets the geometry-save
    // tail know whether to restore maximized after exit.
    std::vector<PlaybackEvent> onFullscreen(bool fullscreen, bool was_maximized);

    // mpv 'osd-dimensions' property change. lw/lh = layout px,
    // pw/ph = physical px. Self-edges silent.
    std::vector<PlaybackEvent> onOsdDims(int lw, int lh, int pw, int ph);

    // mpv buffered-ranges update from the digest thread. Self-edges
    // silent (vector equality).
    std::vector<PlaybackEvent> onBufferedRanges(std::vector<PlaybackBufferedRange> ranges);

    // Display refresh rate update. Self-edges silent.
    std::vector<PlaybackEvent> onDisplayHz(int hz);

    // Drain coord-side actions emitted by the most recent transitions.
    // Coordinator calls after each apply() and fans out to action sinks.
    std::vector<PlaybackAction> consumeActions();

private:
    PlaybackSnapshot s_;
    bool pending_load_ = false;     // set by onLoadStarting, consumed by next end-file
    bool pause_requested_ = false;  // mirrors mpv pause=false latch; gates Starting->Playing alongside !buffering
    bool paused_for_cache_ = false; // raw mpv flag; OR'd with core_idle_ into snapshot.buffering
    bool core_idle_ = false;        // raw mpv flag
    bool frame_available_ = false;  // mpv's video-frame-info AVAILABLE since last FILE_LOADED; truthful first-frame edge
    std::string last_known_item_id_; // Last item Id seen via onLoadStarting; used to set variant_switch_pending on same-Id reload
    std::vector<PlaybackAction> pending_actions_;
};
