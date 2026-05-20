#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Hotkey decision result. Returned by jfn_hotkey_classify_keydown.
//
//   0 None             — forward the event to the browser as normal.
//   1 Shutdown         — caller invokes jfn_shutdown_initiate.
//   2 ToggleFullscreen — caller invokes g_platform.toggle_fullscreen.
//
// The classifier owns the binding table and the video-active gate; the
// caller does nothing except dispatch the returned action.

uint8_t jfn_hotkey_classify_keydown(int32_t windows_key_code, uint32_t modifiers);

#ifdef __cplusplus
}
#endif
