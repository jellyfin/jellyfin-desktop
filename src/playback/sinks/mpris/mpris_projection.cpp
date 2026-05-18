#include "mpris_projection.h"

#include "../../jfn_playback.h"

namespace {

const char* statusName(uint8_t s) {
    switch (s) {
    case 1:  return "Playing";
    case 2:  return "Paused";
    default: return "Stopped";
    }
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
    JfnMprisDerivedC d;
    jfn_mpris_project(static_cast<uint8_t>(playback.phase),
                      playback.seeking,
                      playback.buffering,
                      content.metadata.duration_us,
                      content.pending_rate,
                      &d);
    MprisView v;
    v.playback_status = statusName(d.status);
    v.can_play = d.can_play;
    v.can_pause = d.can_pause;
    v.can_seek = d.can_seek;
    v.can_control = d.can_control;
    // Caller-side metadata pass-through: hold onto MediaMetadata in C++ so it
    // doesn't ride through FFI just to come back unchanged. metadata_active
    // false -> clean transport while no media is loaded.
    v.metadata = d.metadata_active ? content.metadata : MediaMetadata{};
    v.rate = d.rate;
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
