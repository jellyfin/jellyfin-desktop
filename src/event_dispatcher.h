#pragma once

#include "mpv/event.h"

#include <memory>

class BrowserPlaybackSink;

// mpv → coordinator/UI bridge. mpv_digest_thread normalizes mpv events and
// publishes them here; cef_consumer_thread drains the queue, posts playback
// inputs to the coordinator, fans non-playback events out to the active
// browser (execJs) and platform state, and drains the BrowserPlaybackSink
// for coordinator-emitted playback events.

void publish(const MpvEvent& ev);

// Called by run_with_cef before starting the consumer thread so the
// dispatcher knows which sink to drain for playback events.
void set_browser_playback_sink(std::shared_ptr<BrowserPlaybackSink> sink);

void cef_consumer_thread();

// Captured when entering fullscreen so the geometry-save tail can record
// whether to restore as maximized after exiting fullscreen on next launch.
extern bool g_was_maximized_before_fullscreen;
