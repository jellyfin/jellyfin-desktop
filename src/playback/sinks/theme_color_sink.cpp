#include "theme_color_sink.h"

#include "../../theme_color.h"

void ThemeColorSink::deliver(const PlaybackEvent& ev) {
    if (!g_theme_color) return;
    switch (ev.kind) {
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error:
        g_theme_color->setVideoMode(false);
        break;
    default:
        break;
    }
}
