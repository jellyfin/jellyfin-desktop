#include "theme_color_sink.h"

#include "../../theme_color.h"

bool ThemeColorSink::tryPost(const PlaybackEvent& ev) {
    if (!g_theme_color) return true;
    switch (ev.kind) {
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error:
        g_theme_color->setVideoMode(false);
        break;
    default:
        break;
    }
    return true;
}
