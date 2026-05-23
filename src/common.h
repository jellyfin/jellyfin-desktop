#pragma once

#include <atomic>

#include "color.h"

// User's mpv.conf background-color, captured at startup.
extern Color g_video_bg;

#include "platform/platform.h"

#include "playback/jfn_wake_event.h"
#include "shutdown/jfn_shutdown.h"

extern Platform g_platform;

// Set true between PlaybackCoordinatorScope construction and destruction;
// producers gate their `playback::post_*` calls on this to avoid posting
// during shutdown.
inline std::atomic<bool> g_playback_coord_running{false};

// Thin forwarders to the Rust-side shutdown signal (src/playback/src/shutdown.rs).
inline void initiate_shutdown() { jfn_shutdown_initiate(); }
