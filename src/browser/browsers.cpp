#include "browsers.h"

#include "../input/dispatch.h"
#include "../platform/platform.h"
#include "logging.h"

#include <algorithm>

namespace {
// Map opaque JfnCefLayer* back to the CefRefPtr held in Browsers. Iteration
// is fine — Browsers holds at most a handful of layers.
CefLayer* find_by_rs(Browsers* b, JfnCefLayer* rs) {
    if (!b || !rs) return nullptr;
    for (auto& l : b->layers()) {
        if (l->rs() == rs) return l.get();
    }
    return nullptr;
}
}  // namespace

// =====================================================================
// C ABI surface — invoked from Rust business wrappers (jfn-cef
// business_about / business_overlay / business_web) so they can drive
// the still-C++ Browsers registry using opaque JfnCefLayer* tokens.
// =====================================================================

extern "C" JfnCefLayer* jfn_browsers_create(const char* kind) {
    if (!g_browsers) return nullptr;
    auto layer = g_browsers->create(kind ? kind : "");
    return layer ? layer->rs() : nullptr;
}

extern "C" void jfn_browsers_remove(JfnCefLayer* rs) {
    if (auto* l = find_by_rs(g_browsers, rs)) {
        g_browsers->remove(l);
    }
}

extern "C" void jfn_browsers_set_active(JfnCefLayer* rs) {
    if (!g_browsers) return;
    if (!rs) {
        g_browsers->setActive(nullptr);
        return;
    }
    if (auto* l = find_by_rs(g_browsers, rs)) {
        g_browsers->setActive(l);
    }
}

extern "C" JfnCefLayer* jfn_browsers_active() {
    if (!g_browsers) return nullptr;
    auto a = g_browsers->active();
    return a ? a->rs() : nullptr;
}

Browsers* g_browsers = nullptr;

Browsers::Browsers(int lw, int lh, int pw, int ph,
                   double frame_rate, bool use_shared_textures)
    : lw_(lw), lh_(lh), pw_(pw), ph_(ph),
      frame_rate_(frame_rate > 0 ? static_cast<int>(frame_rate + 0.5) : 0),
      use_shared_textures_(use_shared_textures) {
    jfn_cef_set_default_frame_rate(frame_rate_);
    jfn_cef_set_use_shared_textures(use_shared_textures_);
}

Browsers::~Browsers() {
    for (auto& layer : layers_) {
        if (auto* s = layer->surface()) {
            if (g_platform.free_surface) g_platform.free_surface(s);
        }
    }
    layers_.clear();
}

CefRefPtr<CefLayer> Browsers::create(const char* injection_kind) {
    PlatformSurface* surface = g_platform.alloc_surface
        ? g_platform.alloc_surface() : nullptr;
    CefRefPtr<CefLayer> layer = new CefLayer(*this, surface);
    layer->resize(lw_, lh_, pw_, ph_);
    layer->setRefreshRate(frame_rate_);
    if (injection_kind && *injection_kind)
        layer->setInjectionProfileKind(injection_kind);
    layers_.push_back(layer);
    restack_locked();
    return layer;
}

void Browsers::remove(CefLayer* layer) {
    if (!layer) return;
    layer->onDeactivated();
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        if (active_ && active_.get() == layer) active_ = nullptr;
    }
    auto it = std::find_if(layers_.begin(), layers_.end(),
                           [layer](const CefRefPtr<CefLayer>& l) {
                               return l.get() == layer;
                           });
    if (it == layers_.end()) return;
    PlatformSurface* s = (*it)->surface();
    layers_.erase(it);
    if (s && g_platform.free_surface) g_platform.free_surface(s);
    restack_locked();
}

void Browsers::raise_to_top(CefLayer* layer) {
    auto it = std::find_if(layers_.begin(), layers_.end(),
                           [layer](const CefRefPtr<CefLayer>& l) {
                               return l.get() == layer;
                           });
    if (it == layers_.end() || it + 1 == layers_.end()) return;
    CefRefPtr<CefLayer> ref = *it;
    layers_.erase(it);
    layers_.push_back(ref);
    restack_locked();
}

void Browsers::lower_to_bottom(CefLayer* layer) {
    auto it = std::find_if(layers_.begin(), layers_.end(),
                           [layer](const CefRefPtr<CefLayer>& l) {
                               return l.get() == layer;
                           });
    if (it == layers_.end() || it == layers_.begin()) return;
    CefRefPtr<CefLayer> ref = *it;
    layers_.erase(it);
    layers_.insert(layers_.begin(), ref);
    restack_locked();
}

void Browsers::restack_locked() {
    if (!g_platform.restack) return;
    std::vector<PlatformSurface*> ordered;
    ordered.reserve(layers_.size());
    for (auto& l : layers_)
        if (auto* s = l->surface()) ordered.push_back(s);
    g_platform.restack(ordered.data(), ordered.size());
}

void Browsers::setSize(int lw, int lh, int pw, int ph) {
    lw_ = lw; lh_ = lh; pw_ = pw; ph_ = ph;
    for (auto& layer : layers_)
        layer->resize(lw, lh, pw, ph);
}

void Browsers::setScale(double scale) {
    if (scale <= 0 || pw_ <= 0 || ph_ <= 0) return;
    int new_lw = static_cast<int>(pw_ / scale);
    int new_lh = static_cast<int>(ph_ / scale);
    setSize(new_lw, new_lh, pw_, ph_);
}

void Browsers::setRefreshRate(double hz) {
    if (hz <= 0) return;
    frame_rate_ = static_cast<int>(hz + 0.5);
    jfn_cef_set_default_frame_rate(frame_rate_);
    for (auto& layer : layers_)
        layer->setRefreshRate(hz);
}

CefRefPtr<CefLayer> Browsers::active() const {
    std::lock_guard<std::mutex> lk(active_mtx_);
    return active_;
}

bool Browsers::allClosed() const {
    for (auto& l : layers_)
        if (!l->isClosed()) return false;
    return true;
}

void Browsers::closeAll() {
    for (auto& l : layers_) l->closeBrowserForce();
}

void Browsers::waitAllClosed() {
    std::vector<CefRefPtr<CefLayer>> snapshot = layers_;
    for (auto& l : snapshot) l->waitForClose();
}

void Browsers::setActive(CefRefPtr<CefLayer> layer) {
    CefRefPtr<CefLayer> prev;
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        if (active_.get() == layer.get()) return;
        prev = active_;
        active_ = layer;
    }
    LOG_DEBUG(LOG_PLATFORM, "[BROWSERS] setActive prev={} new={}",
              prev ? prev->name().c_str() : "",
              layer ? layer->name().c_str() : "");
    if (prev) {
        prev->setFocus(false);
        prev->onDeactivated();
    }
    if (layer) layer->setFocus(true);

    // Leave-then-move forces the renderer to re-emit OnCursorChange.
    if (layer) {
        auto pos = input::last_mouse_pos();
        if (pos.valid) {
            layer->sendMouseMove(pos.x, pos.y, pos.modifiers, true);
            layer->sendMouseMove(pos.x, pos.y, pos.modifiers, false);
        }
    }
}
