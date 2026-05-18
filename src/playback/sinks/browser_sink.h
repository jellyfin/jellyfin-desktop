#pragma once

#include "../event.h"

// Forwards UI-affecting events to g_web_browser via execJs. Reads only
// from ev.snapshot — never pulls from coord.
class BrowserPlaybackSink final : public PlaybackEventSink {
public:
    bool tryPost(const PlaybackEvent& ev) override;
};
