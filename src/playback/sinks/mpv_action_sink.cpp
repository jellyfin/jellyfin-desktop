#include "mpv_action_sink.h"

#include "../../common.h"

bool MpvActionSink::tryPost(const PlaybackAction& act) {
    switch (act.kind) {
    case PlaybackAction::Kind::ApplyPendingTrackSelectionAndPlay:
        g_mpv.ApplyPendingTrackSelectionAndPlay();
        break;
    }
    return true;
}
