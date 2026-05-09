#pragma once

#include "cef_thread_sinks.h"

// Runs g_mpv.ApplyPendingTrackSelectionAndPlay() on cef_consumer_thread
// in response to ApplyPendingTrackSelectionAndPlay actions emitted by
// the SM on FILE_LOADED. Preserves the prior ordering relative to the
// FILE_LOADED drain.
class MpvActionSink final : public CefThreadActionSink {
protected:
    void deliver(const PlaybackAction& act) override;
};
