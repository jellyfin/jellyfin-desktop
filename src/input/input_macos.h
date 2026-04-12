#pragma once

#ifdef __OBJC__
@class NSView;
#else
typedef void NSView;
#endif

#include "include/internal/cef_types.h"

namespace input::macos {

// Creates the JellyfinInputView (NSView subclass) that captures mouse
// and keyboard for CEF. Caller adds it to the window's content hierarchy.
// Returns nil on failure.
NSView* create_input_view();

// Called via Platform::set_cursor vtable. Applies the cursor on the
// main thread.
void set_cursor(cef_cursor_type_t type);

}  // namespace input::macos
