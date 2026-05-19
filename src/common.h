#pragma once

#include <atomic>

#include "color.h"

// User's mpv.conf background-color, captured at startup.
extern Color g_video_bg;

#include "platform/platform.h"

#include "playback/jfn_wake_event.h"

extern Platform g_platform;

class ThemeColor;
// Set true between PlaybackCoordinatorScope construction and destruction;
// producers gate their `playback::post_*` calls on this to avoid posting
// during shutdown.
extern std::atomic<bool> g_playback_coord_running;

void initiate_shutdown();
extern std::atomic<bool> g_shutting_down;
extern JfnWakeEvent* g_shutdown_event;
extern ThemeColor* g_theme_color;
