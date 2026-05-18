#pragma once

#include "include/cef_browser.h"
#include "include/cef_values.h"
#include "../cef/cef_client.h"

#include <functional>
#include <mutex>
#include <vector>

class WebBrowser;

// Single owner of all live CefLayer instances. Holds the shared display
// state (size, frame rate, shared-texture flag), the active input target,
// and the stack order. The platform sees only PlatformSurface* handles
// (allocated/freed/restacked through it); it never learns which surface
// is "main" or "overlay".
class Browsers {
public:
    Browsers(int lw, int lh, int pw, int ph,
             double frame_rate, bool use_shared_textures);
    ~Browsers();

    Browsers(const Browsers&) = delete;
    Browsers& operator=(const Browsers&) = delete;

    // Allocates a platform surface, builds a CefLayer over it,
    // applies size/refresh/injection, pushes onto top of stack, restacks.
    CefRefPtr<CefLayer> create(CefRefPtr<CefDictionaryValue> injection);

    // Frees the layer's surface, drops it from the stack, restacks.
    // Clears active pointer if it pointed at this layer's browser.
    void remove(CefLayer* layer);

    void raise_to_top(CefLayer* layer);
    void lower_to_bottom(CefLayer* layer);

    const std::vector<CefRefPtr<CefLayer>>& layers() const { return layers_; }

    // Display-state broadcast.
    void setSize(int lw, int lh, int pw, int ph);
    void setRefreshRate(double hz);
    // Re-derive lw/lh from the cached pw/ph using the new display scale,
    // then forward through setSize. Used when only the display scale
    // changes (compositor preferred_scale event, no xdg_toplevel.configure).
    void setScale(double scale);
    int logical_w() const { return lw_; }
    int logical_h() const { return lh_; }
    int physical_w() const { return pw_; }
    int physical_h() const { return ph_; }
    int frame_rate() const { return frame_rate_; }
    bool use_shared_textures() const { return use_shared_textures_; }

    // Input target.
    void setActive(CefRefPtr<CefBrowser> browser);
    CefRefPtr<CefBrowser> active() const;

    // True if every live layer reports closed (or no layers exist).
    bool allClosed() const;

    // Force-close every live browser. Idempotent.
    void closeAll();

    // Block until every layer's CefBrowser has reported OnBeforeClose.
    void waitAllClosed();

    // Run a callback on the underlying CefBrowser of every live layer.
    void forEachBrowser(const std::function<void(CefRefPtr<CefBrowser>)>& fn);

private:
    void restack_locked();  // caller holds no lock; called on CEF UI thread

    int lw_, lh_, pw_, ph_;
    int frame_rate_;
    bool use_shared_textures_;

    std::vector<CefRefPtr<CefLayer>> layers_;  // CEF UI thread only

    mutable std::mutex active_mtx_;
    CefRefPtr<CefBrowser> active_;  // guarded by active_mtx_
};

extern Browsers* g_browsers;

// WebBrowser is the only typed global — playback sinks reach for it to
// dispatch JS into jellyfin-web. Overlay and About are addressed via
// Browsers iteration (allClosed/closeAll/forEachBrowser).
extern WebBrowser* g_web_browser;

// Drops the wrapper's CEF-layer reference from Browsers if Browsers is
// still alive. Used by every business-wrapper dtor.
inline void release_layer(CefLayer* layer) {
    if (g_browsers && layer) g_browsers->remove(layer);
}
