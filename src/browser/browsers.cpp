#include "browsers.h"

#include "../input/dispatch.h"
#include "../platform/platform.h"
#include "logging.h"

#include <algorithm>

Browsers* g_browsers = nullptr;

Browsers::Browsers(int lw, int lh, int pw, int ph,
                   double frame_rate, bool use_shared_textures)
    : lw_(lw), lh_(lh), pw_(pw), ph_(ph),
      frame_rate_(frame_rate > 0 ? static_cast<int>(frame_rate + 0.5) : 0),
      use_shared_textures_(use_shared_textures) {}

Browsers::~Browsers() {
    // Defensive: free any stragglers (owners should normally have released
    // their refs by this point).
    for (auto& layer : layers_) {
        if (auto* s = layer->surface()) {
            if (g_platform.free_surface) g_platform.free_surface(s);
        }
    }
    layers_.clear();
}

CefRefPtr<CefLayer> Browsers::create(CefRefPtr<CefDictionaryValue> injection) {
    PlatformSurface* surface = g_platform.alloc_surface
        ? g_platform.alloc_surface() : nullptr;
    CefRefPtr<CefLayer> layer = new CefLayer(*this, surface);
    layer->resize(lw_, lh_, pw_, ph_);
    layer->setRefreshRate(frame_rate_);
    if (injection)
        layer->setInjectionProfile(std::move(injection));
    layers_.push_back(layer);
    restack_locked();
    return layer;
}

void Browsers::remove(CefLayer* layer) {
    if (!layer) return;
    // Tear down anything inactive-only (popup) before the surface goes.
    layer->onDeactivated();
    // Clear active if it pointed at this layer's browser.
    {
        auto b = layer->browser();
        std::lock_guard<std::mutex> lk(active_mtx_);
        if (b && active_.get() == b.get()) active_ = nullptr;
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

void Browsers::setRefreshRate(double hz) {
    if (hz <= 0) return;
    frame_rate_ = static_cast<int>(hz + 0.5);
    for (auto& layer : layers_)
        layer->setRefreshRate(hz);
}

CefRefPtr<CefBrowser> Browsers::active() const {
    std::lock_guard<std::mutex> lk(active_mtx_);
    return active_;
}

bool Browsers::allClosed() const {
    for (auto& l : layers_)
        if (!l->isClosed()) return false;
    return true;
}

void Browsers::closeAll() {
    for (auto& l : layers_) {
        if (auto b = l->browser()) b->GetHost()->CloseBrowser(true);
    }
}

void Browsers::waitAllClosed() {
    // layers_ is mutated only on the CEF UI thread; close happens via CEF
    // and pulses each layer's condvar. Walk a snapshot so erase-during-
    // close can't trip the iterator.
    std::vector<CefRefPtr<CefLayer>> snapshot = layers_;
    for (auto& l : snapshot) l->waitForClose();
}

void Browsers::forEachBrowser(const std::function<void(CefRefPtr<CefBrowser>)>& fn) {
    for (auto& l : layers_) {
        if (auto b = l->browser()) fn(b);
    }
}

void Browsers::setActive(CefRefPtr<CefBrowser> browser) {
    CefRefPtr<CefBrowser> prev;
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        if (active_.get() == browser.get()) return;
        prev = active_;
        active_ = browser;
    }
    auto name_for = [this](CefRefPtr<CefBrowser> b) -> std::string {
        if (!b) return {};
        for (auto& l : layers_)
            if (auto lb = l->browser(); lb && lb.get() == b.get())
                return l->name();
        return {};
    };
    LOG_DEBUG(LOG_PLATFORM, "[BROWSERS] setActive prev={} new={}",
              name_for(prev).c_str(), name_for(browser).c_str());
    if (prev) {
        prev->GetHost()->SetFocus(false);
        // Notify the prior active layer it's no longer the input target so
        // it can tear down popup state etc.
        for (auto& l : layers_) {
            if (auto lb = l->browser(); lb && lb.get() == prev.get()) {
                l->onDeactivated();
                break;
            }
        }
    }
    if (browser) browser->GetHost()->SetFocus(true);

    // Leave-then-move forces the renderer to re-emit OnCursorChange.
    if (browser) {
        auto pos = input::last_mouse_pos();
        if (pos.valid) {
            CefMouseEvent me{};
            me.x = pos.x; me.y = pos.y; me.modifiers = pos.modifiers;
            browser->GetHost()->SendMouseMoveEvent(me, true);
            browser->GetHost()->SendMouseMoveEvent(me, false);
        }
    }
}
