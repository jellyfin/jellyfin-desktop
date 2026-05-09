#include "mpv_action_sink.h"

#include "../../common.h"

void MpvActionSink::deliver(const PlaybackAction& act) {
    switch (act.kind) {
    case PlaybackAction::Kind::ApplyPendingTrackSelectionAndPlay:
        g_mpv.ApplyPendingTrackSelectionAndPlay();
        break;
    }
}
