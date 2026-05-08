#pragma once

#include "common.h"
#include <functional>
#include <optional>
#include <utility>

// Window-scoped chrome color. Owns no platform or transport detail — it
// tracks the active theme (theme-color updates, overlay buffering, video
// mode) and emits the resolved Color to whatever sink the caller wires up.
// While video plays, the resolved color is g_video_bg (user's mpv.conf) so
// resize letterbox gaps match mpv exactly.
class ThemeColor {
public:
    using Sink = std::function<void(const Color&)>;

    explicit ThemeColor(Sink sink) : sink_(std::move(sink)) { apply(); }

    // Buffered until onOverlayDismissed unlocks, so the chrome doesn't flash
    // through the loading screen color.
    void onThemeColor(const Color& c) {
        current_ = c;
        if (unlocked_) apply();
    }

    void onOverlayDismissed() {
        unlocked_ = true;
        apply();
    }

    void setVideoMode(bool active) {
        if (active == video_active_) return;
        video_active_ = active;
        if (unlocked_) apply();
    }

private:
    void apply() {
        Color c = video_active_ ? g_video_bg : current_;
        if (last_applied_ && *last_applied_ == c) return;
        last_applied_ = c;
        sink_(c);
    }

    Sink sink_;
    bool unlocked_ = false;
    bool video_active_ = false;
    Color current_ = kBgColor;
    std::optional<Color> last_applied_;
};
