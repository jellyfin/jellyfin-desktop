#pragma once

#include "../event.h"

// Resets ThemeColor video mode on terminal events. (Active-true setVideoMode
// fires from web_browser.cpp on metadata arrival; that's not mpv-derived
// and stays out of the playback event stream.)
class ThemeColorSink final : public PlaybackEventSink {
public:
    bool tryPost(const PlaybackEvent& ev) override;
};
