#pragma once

#include "../cef/cef_client.h"

#include <mutex>
#include <vector>

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

    // Allocates a platform surface, builds a CefLayer over it, applies
    // size/refresh/injection kind, pushes onto top of stack, restacks.
    // `injection_kind` is one of "web" / "overlay" / "about" (or empty for
    // no injection).
    CefRefPtr<CefLayer> create(const char* injection_kind);

    // Frees the layer's surface, drops it from the stack, restacks.
    // Clears active pointer if it pointed at this layer.
    void remove(CefLayer* layer);

    void raise_to_top(CefLayer* layer);
    void lower_to_bottom(CefLayer* layer);

    const std::vector<CefRefPtr<CefLayer>>& layers() const { return layers_; }

    // Display-state broadcast.
    void setSize(int lw, int lh, int pw, int ph);
    void setRefreshRate(double hz);
    // Re-derive lw/lh from the cached pw/ph using the new display scale,
    // then forward through setSize.
    void setScale(double scale);
    int logical_w() const { return lw_; }
    int logical_h() const { return lh_; }
    int physical_w() const { return pw_; }
    int physical_h() const { return ph_; }
    int frame_rate() const { return frame_rate_; }
    bool use_shared_textures() const { return use_shared_textures_; }

    // Input target: the layer currently receiving key/mouse events.
    void setActive(CefRefPtr<CefLayer> layer);
    CefRefPtr<CefLayer> active() const;

    bool allClosed() const;
    void closeAll();
    void waitAllClosed();

private:
    void restack_locked();  // CEF UI thread only

    int lw_, lh_, pw_, ph_;
    int frame_rate_;
    bool use_shared_textures_;

    std::vector<CefRefPtr<CefLayer>> layers_;  // CEF UI thread only

    mutable std::mutex active_mtx_;
    CefRefPtr<CefLayer> active_;  // guarded by active_mtx_
};

extern Browsers* g_browsers;

// Drops the layer from Browsers if Browsers is still alive. Used by
// the C++ layers that don't yet route through jfn_browsers_remove.
inline void release_layer(CefLayer* layer) {
    if (g_browsers && layer) g_browsers->remove(layer);
}
