#pragma once

#include "mpv/event.h"

// mpv → CEF/MediaSession bridge. mpv_digest_thread normalizes mpv events and
// publishes them here; cef_consumer_thread drains the queue, fans events out
// to the active browser (execJs), the OS media session, and platform state.

void publish(const MpvEvent& ev);

void cef_consumer_thread();

// Updates the platform's idle inhibit level based on g_playback_state and
// g_media_type. Called from the consumer thread on state transitions, and
// from web_browser when it routes Jellyfin's playback state to native.
void update_idle_inhibit();

// Captured when entering fullscreen so the geometry-save tail can record
// whether to restore as maximized after exiting fullscreen on next launch.
extern bool g_was_maximized_before_fullscreen;
