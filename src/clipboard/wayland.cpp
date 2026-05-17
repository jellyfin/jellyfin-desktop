#include "clipboard/wayland.h"
#include "jfn_clipboard_wayland.h"

#include <utility>

// Thin C++ wrapper preserving the std::function-based API used elsewhere
// in the project. The actual ext-data-control-v1 worker lives in the Rust
// jfn-clipboard-wayland crate.

namespace clipboard_wayland {
namespace {

JfnClipboardWayland* g_clipboard = nullptr;

extern "C" void on_read(void* ctx, const char* text, size_t len) {
    auto* boxed = static_cast<std::function<void(std::string)>*>(ctx);
    if (*boxed) (*boxed)(std::string(text ? text : "", len));
    delete boxed;
}

}  // namespace

void init() {
    if (g_clipboard) return;
    g_clipboard = jfn_clipboard_wayland_init();
}

bool available() {
    return g_clipboard != nullptr;
}

void read_text_async(std::function<void(std::string)> on_done) {
    if (!g_clipboard) {
        if (on_done) on_done(std::string{});
        return;
    }
    auto* boxed = new std::function<void(std::string)>(std::move(on_done));
    jfn_clipboard_wayland_read_text_async(g_clipboard, on_read, boxed);
}

void cleanup() {
    if (!g_clipboard) return;
    jfn_clipboard_wayland_cleanup(g_clipboard);
    g_clipboard = nullptr;
}

}  // namespace clipboard_wayland
