#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "include/internal/cef_types.h"

namespace input::windows {

// Body of the input thread. Creates a child HWND covering mpv's client
// area, attaches thread input so keyboard focus can move to the child,
// then runs the Windows message loop until stop_input_thread() posts
// WM_QUIT. Call from a dedicated std::thread.
void run_input_thread(HWND mpv_hwnd);

// Posts WM_QUIT to the input thread. Safe to call from any thread.
void stop_input_thread();

// Resizes the child input HWND to match mpv's HWND size. Called from
// platform_windows's mpv WndProc hook on WM_SIZE.
void resize_to_parent(int pw, int ph);

// Called via Platform::set_cursor vtable. Safe to call from any thread.
// Updates the stored cursor type; the actual SetCursor() happens on the
// next WM_SETCURSOR.
void set_cursor(cef_cursor_type_t type);

}  // namespace input::windows
