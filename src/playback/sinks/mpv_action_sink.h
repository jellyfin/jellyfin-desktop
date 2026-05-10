#pragma once

#include "queued_sink.h"

// Runs g_mpv.ApplyPendingTrackSelectionAndPlay() on cef_consumer_thread
// in response to ApplyPendingTrackSelectionAndPlay actions emitted by
// the SM on FILE_LOADED. Preserves the prior ordering relative to the
// FILE_LOADED drain.
class MpvActionSink final : public QueuedActionSink {
protected:
    void deliver(const PlaybackAction& act) override;
};
