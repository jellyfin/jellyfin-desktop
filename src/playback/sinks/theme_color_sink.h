#pragma once

#include "cef_thread_sinks.h"

// Resets ThemeColor video mode on terminal events. (Active-true setVideoMode
// fires from web_browser.cpp on metadata arrival; that's not mpv-derived
// and stays out of the playback event stream.)
class ThemeColorSink final : public CefThreadPlaybackSink {
protected:
    void deliver(const PlaybackEvent& ev) override;
};
