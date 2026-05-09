#pragma once

#include "queued_sink.h"

// Resets ThemeColor video mode on terminal events. (Active-true setVideoMode
// fires from web_browser.cpp on metadata arrival; that's not mpv-derived
// and stays out of the playback event stream.)
class ThemeColorSink final : public QueuedPlaybackSink {
protected:
    void deliver(const PlaybackEvent& ev) override;
};
