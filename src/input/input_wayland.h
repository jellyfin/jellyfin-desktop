#pragma once

#include <wayland-client.h>
#include "include/internal/cef_types.h"

struct wp_cursor_shape_manager_v1;

namespace input::wayland {

// Initializes xkb context. Must be called before any listener fires.
// `display` and `queue` are borrowed — caller owns their lifetime.
void init(wl_display* display, wl_event_queue* queue);

// Called by platform_wayland's registry binding when a wl_seat appears.
// Stores the seat and attaches the seat listener (which will in turn
// attach pointer/keyboard listeners as the compositor advertises them).
void attach_seat(wl_seat* seat);

// Called when wp_cursor_shape_manager_v1 binds via the registry.
// Used to create a cursor shape device on demand in set_cursor.
void attach_cursor_shape_manager(wp_cursor_shape_manager_v1* mgr);

// Starts the input thread that polls wl_display and the shutdown wake
// event, dispatching the dedicated queue until shutdown.
void start_input_thread();

// Joins the input thread, destroys seat/keyboard/pointer/xkb resources
// and the cursor shape device. Safe to call even if start_input_thread
// was never called.
void cleanup();

// Called via Platform::set_cursor vtable. Records the desired cursor
// and applies it to the pointer if the pointer currently has a serial.
void set_cursor(cef_cursor_type_t type);

}  // namespace input::wayland
