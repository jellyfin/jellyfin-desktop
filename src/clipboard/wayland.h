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
// Thin C++ wrapper over the Rust jfn-clipboard-wayland crate, preserving
// the std::function-based API used elsewhere in the project.

#include <functional>
#include <string>
#include <utility>

#include "jfn_clipboard_wayland.h"

namespace clipboard_wayland {

namespace detail {
inline JfnClipboardWayland*& instance() {
    static JfnClipboardWayland* g = nullptr;
    return g;
}

extern "C" inline void on_read(void* ctx, const char* text, size_t len) {
    auto* boxed = static_cast<std::function<void(std::string)>*>(ctx);
    if (*boxed) (*boxed)(std::string(text ? text : "", len));
    delete boxed;
}
}  // namespace detail

inline void init() {
    auto& g = detail::instance();
    if (g) return;
    g = jfn_clipboard_wayland_init();
}

inline bool available() { return detail::instance() != nullptr; }

inline void read_text_async(std::function<void(std::string)> on_done) {
    auto* g = detail::instance();
    if (!g) {
        if (on_done) on_done(std::string{});
        return;
    }
    auto* boxed = new std::function<void(std::string)>(std::move(on_done));
    jfn_clipboard_wayland_read_text_async(g, detail::on_read, boxed);
}

inline void cleanup() {
    auto& g = detail::instance();
    if (!g) return;
    jfn_clipboard_wayland_cleanup(g);
    g = nullptr;
}

}  // namespace clipboard_wayland
