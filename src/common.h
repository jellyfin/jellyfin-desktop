#pragma once

#include <atomic>
#include <cstdint>

constexpr char hexdigit(uint32_t c, int i) {
    uint8_t n = (c >> (20 - i * 4)) & 0xF;
    return n < 10 ? '0' + n : 'a' + (n - 10);
}

struct Color {
    uint32_t rgb;
    uint8_t r, g, b;
    char hex[8];
    constexpr Color(uint32_t c) :
        rgb(c),
        r((c >> 16) & 0xFF),
        g((c >> 8) & 0xFF),
        b(c & 0xFF),
        hex{'#', hexdigit(c,0), hexdigit(c,1), hexdigit(c,2),
            hexdigit(c,3), hexdigit(c,4), hexdigit(c,5), '\0'} {}
};

// Startup background color (loading screen / overlay).
constexpr Color kBgColor{0x101010};
// Playback background color (behind video).
constexpr Color kVideoBgColor{0x000000};

#include "platform/platform.h"
#include "mpv/handle.h"
#include "player/media_session.h"

class WakeEvent;

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
