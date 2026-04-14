#pragma once

#include "platform/platform.h"
#include <string>
#include <cstdio>

// Follows <meta name="theme-color"> from web content.
// Locked while overlay is visible, then tracks the meta tag.
class TitlebarColor {
public:
    TitlebarColor(Platform& platform, bool enabled)
        : platform_(platform), enabled_(enabled) {}

    void onThemeColor(const std::string& color) {
        if (!enabled_) return;
        current_ = color;
        if (unlocked_)
            applyHex(color);
    }

    void onOverlayDismissed() {
        unlocked_ = true;
        if (!current_.empty())
            applyHex(current_);
    }

private:
    void applyHex(const std::string& color) {
        if (color.size() < 4 || color[0] != '#') return;
        unsigned r = 0, g = 0, b = 0;
        if (color.size() == 7) {
            sscanf(color.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
        } else if (color.size() == 4) {
            sscanf(color.c_str() + 1, "%1x%1x%1x", &r, &g, &b);
            r *= 0x11; g *= 0x11; b *= 0x11;
        }
        platform_.set_titlebar_color(r, g, b);
    }

    Platform& platform_;
    bool enabled_;
    bool unlocked_ = false;
    std::string current_;
};
