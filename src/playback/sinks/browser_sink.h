#pragma once

#include "cef_thread_sinks.h"

// Forwards UI-affecting events to g_web_browser via execJs. Reads only
// from ev.snapshot — never pulls from coord.
class BrowserPlaybackSink final : public CefThreadPlaybackSink {
protected:
    void deliver(const PlaybackEvent& ev) override;
};
