#pragma once

#include <xcb/xcb.h>
#include <functional>
#include "include/internal/cef_types.h"

namespace input::x11 {

void init(xcb_connection_t* conn, xcb_screen_t* screen, xcb_window_t window);

// Register a callback for ConfigureNotify on the parent window.
// Fires when mpv's window moves or resizes (for overlay repositioning).
void set_configure_callback(std::function<void()> cb);

// Register a callback invoked when shutdown is detected (window destroyed,
// or shutdown event fired). Called from the input thread before it exits.
void set_shutdown_callback(std::function<void()> cb);

void start_input_thread();
void cleanup();
void set_cursor(cef_cursor_type_t type);

}  // namespace input::x11
