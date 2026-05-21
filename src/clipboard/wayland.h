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
// Thin C++ wrapper over the Rust jfn-clipboard-wayland crate.

#include <stddef.h>

#include "jfn_clipboard_wayland.h"

namespace clipboard_wayland {

namespace detail {
inline JfnClipboardWayland*& instance() {
    static JfnClipboardWayland* g = nullptr;
    return g;
}

// Heap-alloced shim that lets us attach a dtor to the otherwise-bare
// (cb, ctx) pair accepted by the Rust crate. on_read() fires cb, then
// dtor, then frees the shim.
struct ReadShim {
    void (*cb)(void*, const char*, size_t);
    void* ctx;
    void (*dtor)(void*);
};

extern "C" inline void on_read(void* ctx, const char* text, size_t len) {
    auto* shim = static_cast<ReadShim*>(ctx);
    if (shim->cb) shim->cb(shim->ctx, text, len);
    if (shim->dtor) shim->dtor(shim->ctx);
    delete shim;
}
}  // namespace detail

inline void init() {
    auto& g = detail::instance();
    if (g) return;
    g = jfn_clipboard_wayland_init();
}

inline bool available() { return detail::instance() != nullptr; }

inline void read_text_async(void (*cb)(void*, const char*, size_t),
                            void* ctx,
                            void (*dtor)(void*)) {
    auto* g = detail::instance();
    if (!g) {
        if (cb) cb(ctx, "", 0);
        if (dtor) dtor(ctx);
        return;
    }
    auto* shim = new detail::ReadShim{cb, ctx, dtor};
    jfn_clipboard_wayland_read_text_async(g, detail::on_read, shim);
}

inline void cleanup() {
    auto& g = detail::instance();
    if (!g) return;
    jfn_clipboard_wayland_cleanup(g);
    g = nullptr;
}

}  // namespace clipboard_wayland
