#include "hotkeys.h"

#include "input.h"
#include "../common.h"
#include "../platform/platform.h"

namespace input {
namespace {

// Fullscreen is only meaningful when the video player is the active content.
// Music playback ignores fullscreen hotkeys; a paused video still counts as
// "active" because the user may want to toggle fullscreen while paused.
bool video_player_active() {
    return g_media_type.load(std::memory_order_relaxed) == MediaType::Video &&
           g_playback_state.load(std::memory_order_relaxed) != PlaybackState::Stopped;
}

}  // namespace

bool hotkey_try_consume(const KeyEvent& e) {
    if (e.action != KeyAction::Down) return false;

    // Alt+F4: close window
    if (e.code == KeyCode::F4 && (e.modifiers & EVENTFLAG_ALT_DOWN)) {
        initiate_shutdown();
        return true;
    }

    if (e.code == KeyCode::F || e.code == KeyCode::F11) {
        if (!video_player_active()) return false;
        g_platform.toggle_fullscreen();
        return true;
    }
    return false;
}

}  // namespace input
