// extern "C" lifecycle bridges over clipboard/wayland.h's header-only
// inline namespace so jfn_wayland::lifecycle can drive init/cleanup
// without depending on the C++ static instance() pointer directly.

#include "wayland.h"

extern "C" void jfn_clipboard_wayland_lifecycle_init() {
    clipboard_wayland::init();
}

extern "C" bool jfn_clipboard_wayland_lifecycle_available() {
    return clipboard_wayland::available();
}

extern "C" void jfn_clipboard_wayland_lifecycle_cleanup() {
    clipboard_wayland::cleanup();
}
