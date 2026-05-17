#pragma once

#include <wayland-client.h>
#include "include/internal/cef_types.h"

namespace input::wayland {

// Initialize the Rust input layer. `display` is borrowed; caller retains
// ownership. Binds its own wl_seat + wp_cursor_shape_manager_v1 internally.
void init(wl_display* display);

// Start the Rust-owned input thread.
void start_input_thread();

// Tear down. Caller must already have signalled the global shutdown wake
// event so the input thread exits its poll().
void cleanup();

// Set the cursor shape from a CEF cursor type. Safe from any thread.
void set_cursor(cef_cursor_type_t type);

}  // namespace input::wayland
