#pragma once

#include "../event.h"

// Runs g_mpv.ApplyPendingTrackSelectionAndPlay() in response to
// ApplyPendingTrackSelectionAndPlay actions emitted by the SM on
// FILE_LOADED. Preserves the prior ordering relative to the FILE_LOADED
// drain (coordinator emits events first, actions after, all on the
// coordinator worker thread).
class MpvActionSink final : public PlaybackActionSink {
public:
    bool tryPost(const PlaybackAction& act) override;
};
