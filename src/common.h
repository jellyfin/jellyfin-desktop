#pragma once

#include <atomic>

#include "color.h"

// User's mpv.conf background-color, captured at startup.
extern Color g_video_bg;

#include "platform/platform.h"
#include "mpv/handle.h"

class WakeEvent;

extern MpvHandle g_mpv;
extern Platform g_platform;

class ThemeColor;
class PlaybackCoordinator;
extern PlaybackCoordinator* g_playback_coord;

void initiate_shutdown();
extern std::atomic<bool> g_shutting_down;
extern WakeEvent g_shutdown_event;
extern ThemeColor* g_theme_color;
