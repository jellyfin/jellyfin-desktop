#include "mpris_projection.h"

namespace {

// MPRIS only recognizes Playing/Paused/Stopped. Pre-roll (phase=Starting)
// reflects user intent: the user pressed play, so PlaybackStatus reads
// Playing. The fact that frames aren't actually rolling yet is signalled
// through Rate=0 in project(). Paused means an explicit user pause.
const char* statusFor(const PlaybackSnapshot& s) {
    switch (s.phase) {
    case PlaybackPhase::Playing:  return "Playing";
    case PlaybackPhase::Starting: return "Playing";
    case PlaybackPhase::Paused:   return "Paused";
    default:                      return "Stopped";
}
}

bool isActive(PlaybackPhase p) {
    return p == PlaybackPhase::Playing
        || p == PlaybackPhase::Paused
        || p == PlaybackPhase::Starting;
}

}  // namespace

bool MprisView::operator==(const MprisView& o) const {
    return playback_status == o.playback_status
        && can_play == o.can_play
        && can_pause == o.can_pause
        && can_seek == o.can_seek
        && can_control == o.can_control
        && metadata == o.metadata
        && rate == o.rate
        && volume == o.volume
        && can_go_next == o.can_go_next
        && can_go_previous == o.can_go_previous;
}

MprisView project(const PlaybackSnapshot& playback,
                  const MprisContent& content) {
    MprisView v;
    v.playback_status = statusFor(playback);

    bool active = isActive(playback.phase);
    v.can_play = active;
    // CanPause is true while we're committed to playing — Playing or
    // Starting (user already pressed play). Paused exposes Play, not
    // Pause; Stopped exposes neither.
    v.can_pause = playback.phase == PlaybackPhase::Playing
               || playback.phase == PlaybackPhase::Starting;
    v.can_control = active;

    // Metadata is suppressed while not active so MPRIS clients see a
    // clean transport when nothing is loaded. content.metadata stays
    // intact for the next active transition; the projection just hides
    // it for one render pass.
    v.metadata = active ? content.metadata : MediaMetadata{};
    v.can_seek = active && v.metadata.duration_us > 0;

    // Rate reflects actual frame motion, not user intent. Anything
    // other than steady playback (pre-roll, seek, buffer underrun)
    // pins it to 0 so MPRIS clients don't extrapolate position.
    bool rolling = playback.phase == PlaybackPhase::Playing
                && !playback.seeking
                && !playback.buffering;
    v.rate = rolling ? content.pending_rate : 0.0;
    v.volume = content.volume;
    v.can_go_next = content.can_go_next;
    v.can_go_previous = content.can_go_previous;
    return v;
}

std::vector<const char*> diff(const MprisView& a, const MprisView& b) {
    std::vector<const char*> out;
    if (a.playback_status != b.playback_status) out.push_back("PlaybackStatus");
    if (a.can_play       != b.can_play)         out.push_back("CanPlay");
    if (a.can_pause      != b.can_pause)        out.push_back("CanPause");
    if (a.can_seek       != b.can_seek)         out.push_back("CanSeek");
    if (a.can_control    != b.can_control)      out.push_back("CanControl");
    if (!(a.metadata == b.metadata))            out.push_back("Metadata");
    if (a.rate           != b.rate)             out.push_back("Rate");
    if (a.volume         != b.volume)           out.push_back("Volume");
    if (a.can_go_next    != b.can_go_next)      out.push_back("CanGoNext");
    if (a.can_go_previous != b.can_go_previous) out.push_back("CanGoPrevious");
    return out;
}
