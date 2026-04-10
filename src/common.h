#pragma once

#include <atomic>

#include "include/cef_base.h"

#include "platform.h"
#include "mpv/handle.h"
#include "player/media_session.h"

class Client;
class OverlayClient;
class WakeEvent;

extern CefRefPtr<Client> g_client;
extern CefRefPtr<OverlayClient> g_overlay_client;
extern MpvHandle g_mpv;
extern Platform g_platform;
// Cross-thread state: written from mpv event loop / CEF IPC thread,
// read from input and rendering threads.
extern std::atomic<MediaType> g_media_type;
extern std::atomic<PlaybackState> g_playback_state;

class MediaSessionThread;
class TitlebarColor;

void initiate_shutdown();
extern std::atomic<bool> g_shutting_down;
extern WakeEvent g_shutdown_event;
extern MediaSessionThread* g_media_session;
extern TitlebarColor* g_titlebar_color;

// Display refresh rate (Hz) — updated from mpv's display-fps property observation.
// Defaults to 60 until mpv reports the actual value.
extern std::atomic<int> g_display_hz;
