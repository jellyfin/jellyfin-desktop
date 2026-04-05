#pragma once

#include <mpv/client.h>
#include <atomic>

#include "include/cef_base.h"

#include "platform.h"

class Client;
class OverlayClient;
class WakeEvent;

extern CefRefPtr<Client> g_client;
extern CefRefPtr<OverlayClient> g_overlay_client;
extern mpv_handle* g_mpv;
extern Platform g_platform;

class MediaSessionThread;

void initiate_shutdown();
extern std::atomic<bool> g_shutting_down;
extern WakeEvent g_shutdown_event;
extern MediaSessionThread* g_media_session;
