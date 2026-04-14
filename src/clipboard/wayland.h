#pragma once

// Wayland clipboard (CLIPBOARD selection) read path.
//
// Wayland-native apps' clipboard contents aren't reachable via CEF under
// --ozone-platform=x11 (the read path has no way to observe external
// selection changes). We run an independent wl_data_device_manager on our
// own wl_event_queue and worker thread so the context menu's Paste can
// read text directly from the compositor — no bridging through Chromium.
//
// Writes still go through CEF's frame->Copy() which works correctly on
// every platform we care about, so this is read-only.
//
// Entirely event-driven: the worker thread poll()s the display fd, its
// own wake event, and any active pipe fds from outstanding receives.
// No timeouts, no polling loops.

#include <functional>
#include <string>

namespace clipboard_wayland {

// Initialize the clipboard worker: opens its own wl_display connection,
// binds ext-data-control-v1, starts a dedicated worker thread. No-op on
// compositors that don't advertise the protocol — callers should check
// available() afterwards and fall back to CEF's native clipboard path.
void init();

// True if init() succeeded and the clipboard worker is running. Platform
// code should null out g_platform.clipboard_read_text_async when this is
// false so the context menu can route through CEF's native frame->Paste()
// instead (which works on compositors with good XWayland clipboard
// bridging, notably Mutter/GNOME).
bool available();

// Start an async read of the current CLIPBOARD selection as UTF-8 text.
// The callback fires on the clipboard worker thread when the source has
// finished writing, or with an empty string if nothing text-shaped is on
// the clipboard. Safe to call from any thread.
void read_text_async(std::function<void(std::string)> on_done);

// Join the worker thread and destroy clipboard Wayland objects.
void cleanup();

}  // namespace clipboard_wayland
