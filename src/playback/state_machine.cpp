#include "state_machine.h"

#include <utility>

namespace {

bool is_active_phase(PlaybackPhase p) {
    return p == PlaybackPhase::Starting
        || p == PlaybackPhase::Playing
        || p == PlaybackPhase::Paused;
}

// Whether mpv has produced a real first frame yet. We only block on
// frame_available for Video — audio-only and Unknown have no
// equivalent signal, so trust the buffering-clear path instead.
bool ready_to_play(MediaType type, bool frame_available) {
    return type != MediaType::Video || frame_available;
}

// Promotes the snapshot to Playing and emits Started. Single source of
// truth for the "real playback now rolling" transition. Clears
// variant_switch_pending here so the flag spans the whole reload
// window — set by onLoadStarting, cleared only when first-frame
// playback actually starts (or a terminal end-file).
void transitionToPlaying(PlaybackSnapshot& s, std::vector<PlaybackEvent>& out) {
    s.phase = PlaybackPhase::Playing;
    s.variant_switch_pending = false;
    out.push_back({PlaybackEvent::Kind::Started});
}

}  // namespace

std::vector<PlaybackEvent> PlaybackStateMachine::onFileLoaded() {
    s_.presence = PlayerPresence::Present;
    s_.phase = PlaybackPhase::Starting;
    s_.seeking = false;
    s_.buffering = paused_for_cache_ || core_idle_;
    // variant_switch_pending intentionally NOT cleared here — mpv loads
    // paused after FILE_LOADED, and we want the flag (and the JS-side
    // pause indicator it drives) to span until the new variant's first
    // frame promotes to Playing via transitionToPlaying.
    pending_load_ = false;
    pause_requested_ = false;
    frame_available_ = false;
    // mpv loads paused; queue the pending vid/aid/sid selection + unpause.
    // Action sinks fire this on the cef consumer thread (preserves the
    // old ordering relative to FILE_LOADED draining).
    pending_actions_.push_back(
        {PlaybackAction::Kind::ApplyPendingTrackSelectionAndPlay});
    return {{PlaybackEvent::Kind::TrackLoaded}};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onLoadStarting(std::string item_id) {
    pending_load_ = true;
    s_.presence = PlayerPresence::Present;
    s_.phase = PlaybackPhase::Starting;
    s_.seeking = false;
    s_.buffering = paused_for_cache_ || core_idle_;
    s_.variant_switch_pending =
        !item_id.empty() && item_id == last_known_item_id_;
    if (!item_id.empty()) last_known_item_id_ = std::move(item_id);
    pause_requested_ = false;
    frame_available_ = false;
    return {{PlaybackEvent::Kind::TrackLoaded}};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onPauseChanged(bool paused) {
    if (s_.presence == PlayerPresence::None) return {};
    if (s_.phase == PlaybackPhase::Stopped) return {};

    pause_requested_ = !paused;

    if (paused) {
        if (s_.phase == PlaybackPhase::Paused) return {};
        s_.phase = PlaybackPhase::Paused;
        return {{PlaybackEvent::Kind::Paused}};
    }

    // pause=false. Leaving Starting requires the real first frame to have
    // been observed (video-frame-info AVAILABLE). For audio-only content
    // we don't have that signal; fall back to !buffering. From Paused,
    // resume is immediate.
    if (s_.phase == PlaybackPhase::Playing) return {};
    if (s_.phase == PlaybackPhase::Paused) {
        std::vector<PlaybackEvent> out;
        transitionToPlaying(s_, out);
        return out;
    }
    if (s_.phase == PlaybackPhase::Starting) {
        if (s_.buffering) return {};
        if (!ready_to_play(s_.media_type, frame_available_)) return {};
        std::vector<PlaybackEvent> out;
        transitionToPlaying(s_, out);
        return out;
    }
    return {};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onEndFile(
    EndReason reason, std::string error_message)
{
    std::vector<PlaybackEvent> out;

    if (s_.seeking) {
        s_.seeking = false;
        PlaybackEvent e{PlaybackEvent::Kind::SeekingChanged};
        e.flag = false;
        out.push_back(e);
    }
    if (s_.buffering) {
        s_.buffering = false;
        PlaybackEvent e{PlaybackEvent::Kind::BufferingChanged};
        e.flag = false;
        out.push_back(e);
    }

    // Track-switch path: pending_load means a fresh loadfile is in
    // flight. Eat the EOF/cancel so consumers don't see a Stopped
    // flicker between tracks. Errors still terminate so failures
    // surface to the user.
    if (pending_load_ && reason != EndReason::Error) {
        pending_load_ = false;
        s_.presence = PlayerPresence::Present;
        s_.phase = PlaybackPhase::Starting;
        // Preserve position_us — onLoadStarting seeded it for the new
        // track. Resetting here would erase the seed before the new
        // FILE_LOADED arrives.
        return out;
    }

    pending_load_ = false;
    pause_requested_ = false;
    s_.presence = PlayerPresence::None;
    s_.phase = PlaybackPhase::Stopped;
    s_.position_us = 0;
    s_.variant_switch_pending = false;
    last_known_item_id_.clear();

    PlaybackEvent terminal;
    switch (reason) {
    case EndReason::Eof:      terminal.kind = PlaybackEvent::Kind::Finished; break;
    case EndReason::Canceled: terminal.kind = PlaybackEvent::Kind::Canceled; break;
    case EndReason::Error:
        terminal.kind = PlaybackEvent::Kind::Error;
        terminal.error_message = std::move(error_message);
        break;
    }
    out.push_back(std::move(terminal));
    return out;
}

std::vector<PlaybackEvent> PlaybackStateMachine::onSeekingChanged(bool seeking) {
    if (!is_active_phase(s_.phase)) {
        // No player → ignore. Snapshot already has seeking=false from terminal.
        return {};
    }
    if (s_.seeking == seeking) return {};
    s_.seeking = seeking;
    PlaybackEvent e{PlaybackEvent::Kind::SeekingChanged};
    e.flag = seeking;
    return {e};
}

namespace {

// Recompute snapshot.buffering = paused_for_cache || core_idle.
// Returns the events emitted by the transition (BufferingChanged and
// optional Started when the buffer clears during pre-roll).
std::vector<PlaybackEvent> applyBufferingChange(PlaybackSnapshot& s,
                                                bool pfc, bool core_idle,
                                                bool pause_requested,
                                                bool frame_available)
{
    bool combined = pfc || core_idle;
    if (s.buffering == combined) return {};
    s.buffering = combined;

    std::vector<PlaybackEvent> out;
    PlaybackEvent be{PlaybackEvent::Kind::BufferingChanged};
    be.flag = combined;
    out.push_back(be);

    if (!combined && s.phase == PlaybackPhase::Starting && pause_requested
        && ready_to_play(s.media_type, frame_available))
    {
        transitionToPlaying(s, out);
    }
    return out;
}

}  // namespace

std::vector<PlaybackEvent> PlaybackStateMachine::onPausedForCache(bool pfc) {
    if (paused_for_cache_ == pfc) return {};
    paused_for_cache_ = pfc;
    if (!is_active_phase(s_.phase)) return {};
    return applyBufferingChange(s_, paused_for_cache_, core_idle_,
                                pause_requested_, frame_available_);
}

std::vector<PlaybackEvent> PlaybackStateMachine::onCoreIdle(bool core_idle) {
    if (core_idle_ == core_idle) return {};
    core_idle_ = core_idle;
    if (!is_active_phase(s_.phase)) return {};
    return applyBufferingChange(s_, paused_for_cache_, core_idle_,
                                pause_requested_, frame_available_);
}

std::vector<PlaybackEvent> PlaybackStateMachine::onPosition(int64_t position_us) {
    if (s_.position_us == position_us && !s_.seeking) return {};
    s_.position_us = position_us;
    std::vector<PlaybackEvent> out;
    if (s_.seeking) {
        s_.seeking = false;
        PlaybackEvent se{PlaybackEvent::Kind::SeekingChanged};
        se.flag = false;
        out.push_back(se);
    }
    out.push_back({PlaybackEvent::Kind::PositionChanged});
    return out;
}

std::vector<PlaybackEvent> PlaybackStateMachine::onSpeed(double rate) {
    if (s_.rate == rate) return {};
    s_.rate = rate;
    return {{PlaybackEvent::Kind::RateChanged}};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onDuration(int64_t duration_us) {
    if (s_.duration_us == duration_us) return {};
    s_.duration_us = duration_us;
    return {{PlaybackEvent::Kind::DurationChanged}};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onFullscreen(bool fullscreen, bool was_maximized) {
    if (s_.fullscreen == fullscreen) return {};
    s_.fullscreen = fullscreen;
    s_.maximized_before_fullscreen = fullscreen ? was_maximized : false;
    return {{PlaybackEvent::Kind::FullscreenChanged}};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onOsdDims(int lw, int lh, int pw, int ph) {
    if (s_.layout_w == lw && s_.layout_h == lh && s_.pixel_w == pw && s_.pixel_h == ph)
        return {};
    s_.layout_w = lw;
    s_.layout_h = lh;
    s_.pixel_w = pw;
    s_.pixel_h = ph;
    return {{PlaybackEvent::Kind::OsdDimsChanged}};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onBufferedRanges(std::vector<PlaybackBufferedRange> ranges) {
    if (ranges.size() == s_.buffered.size()) {
        bool same = true;
        for (size_t i = 0; i < ranges.size(); ++i) {
            if (ranges[i].start_ticks != s_.buffered[i].start_ticks ||
                ranges[i].end_ticks   != s_.buffered[i].end_ticks) {
                same = false;
                break;
            }
        }
        if (same) return {};
    }
    s_.buffered = std::move(ranges);
    return {{PlaybackEvent::Kind::BufferedRangesChanged}};
}

std::vector<PlaybackEvent> PlaybackStateMachine::onDisplayHz(double hz) {
    if (s_.display_hz == hz) return {};
    s_.display_hz = hz;
    return {{PlaybackEvent::Kind::DisplayHzChanged}};
}

std::vector<PlaybackAction> PlaybackStateMachine::consumeActions() {
    std::vector<PlaybackAction> out;
    out.swap(pending_actions_);
    return out;
}

std::vector<PlaybackEvent> PlaybackStateMachine::onVideoFrameAvailable(bool available) {
    if (frame_available_ == available) return {};
    frame_available_ = available;
    if (!available) return {};
    if (s_.phase != PlaybackPhase::Starting) return {};
    if (!pause_requested_) return {};
    if (s_.buffering) return {};
    std::vector<PlaybackEvent> out;
    transitionToPlaying(s_, out);
    return out;
}

std::vector<PlaybackEvent> PlaybackStateMachine::onMediaType(MediaType type) {
    if (s_.media_type == type) return {};
    s_.media_type = type;
    std::vector<PlaybackEvent> out{{PlaybackEvent::Kind::MediaTypeChanged}};
    // Switching to Audio relaxes the frame-available gate; promote now if
    // the rest of the conditions are met.
    if (s_.phase == PlaybackPhase::Starting && pause_requested_ && !s_.buffering
        && ready_to_play(s_.media_type, frame_available_))
    {
        transitionToPlaying(s_, out);
    }
    return out;
}
