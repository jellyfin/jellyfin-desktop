#pragma once

#include <atomic>

#include "include/cef_base.h"

#include "platform.h"
#include "mpv_handle.h"

class Client;
class OverlayClient;
class WakeEvent;

extern CefRefPtr<Client> g_client;
extern CefRefPtr<OverlayClient> g_overlay_client;
extern MpvHandle g_mpv;
extern Platform g_platform;

class MediaSessionThread;
class TitlebarColor;

void initiate_shutdown();
extern std::atomic<bool> g_shutting_down;
extern WakeEvent g_shutdown_event;
extern MediaSessionThread* g_media_session;
extern TitlebarColor* g_titlebar_color;
