#pragma once

#include "../media_session.h"
#include "../../playback/event.h"

#include <string>
#include <vector>

// MPRIS-local content state. Owned by MprisBackend because no other
// component is the authoritative source: metadata/artwork/canGo flags
// flow from the JS IPC handler in web_browser; volume and pending rate
// flow from MPRIS clients writing the bus-exposed properties or from
// rate setter calls. Playback state (Playing/Paused/Stopped, seeking,
// buffering) is NOT part of this struct — that belongs to the
// PlaybackCoordinator snapshot, fed into project() at compute time.
struct MprisContent {
    MediaMetadata metadata;
    double pending_rate = 1.0;
    double volume = 1.0;
    bool can_go_next = false;
    bool can_go_previous = false;
};

// Fully derived MPRIS Player-interface property values. Every field
// corresponds 1:1 to a property exposed on org.mpris.MediaPlayer2.Player.
// `Position` is excluded because MPRIS specifies it as polled, not
// signaled, so it never participates in the diff/emit pipeline.
struct MprisView {
    std::string playback_status = "Stopped";
    bool can_play = false;
    bool can_pause = false;
    bool can_seek = false;
    bool can_control = false;
    MediaMetadata metadata;
    double rate = 1.0;
    double volume = 1.0;
    bool can_go_next = false;
    bool can_go_previous = false;

    bool operator==(const MprisView& o) const;
};

// Pure projection. Encodes every rule that today is scattered across
// MprisBackend setters and prop_get_* getters:
//   - PlaybackStatus from playback.phase
//   - CanPlay/CanPause/CanSeek/CanControl from phase + duration
//   - Metadata cleared while phase==Stopped (was inline in setPlaybackState)
//   - Rate locked to 0 while seeking|buffering (was syncRate)
MprisView project(const PlaybackSnapshot& playback,
                  const MprisContent& content);

// Diff prev vs next view. Returns the stable MPRIS property-name string
// literals whose value changed. Used to drive
// sd_bus_emit_properties_changed; no setter ever names a property
// directly.
std::vector<const char*> diff(const MprisView& prev, const MprisView& next);
