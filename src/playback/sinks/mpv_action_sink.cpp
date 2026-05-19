#include "mpv_action_sink.h"

#include "../../mpv/jfn_mpv_api.h"

bool MpvActionSink::tryPost(const PlaybackAction& act) {
    switch (act.kind) {
    case PlaybackAction::Kind::ApplyPendingTrackSelectionAndPlay:
        jfn_mpv_apply_pending_track_selection_and_play();
        break;
    }
    return true;
}
