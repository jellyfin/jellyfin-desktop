#pragma once

#include "mpv/event.h"

#include <memory>
#include <vector>

class CefThreadPlaybackSink;
class CefThreadActionSink;

// mpv → coordinator bridge. mpv_digest_thread normalizes mpv events and
// publishes them here; cef_consumer_thread routes every playback-relevant
// MpvEvent to the PlaybackCoordinator and pumps registered cef-thread
// sinks (BrowserPlaybackSink, IdleInhibitSink, ThemeColorSink,
// MpvActionSink) so they execute on the cef consumer thread.

void publish(const MpvEvent& ev);

// Called by run_with_cef before starting the consumer thread so the
// dispatcher knows which sinks to pump. Sinks must outlive the consumer
// thread.
void register_cef_thread_sinks(
    std::vector<std::shared_ptr<CefThreadPlaybackSink>> event_sinks,
    std::vector<std::shared_ptr<CefThreadActionSink>> action_sinks);

void cef_consumer_thread();

// Set by BrowserPlaybackSink on FullscreenChanged events; read by the
// geometry-save tail in main.cpp at shutdown so we can record whether to
// restore as maximized after exiting fullscreen on next launch.
extern bool g_was_maximized_before_fullscreen;
