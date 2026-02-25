#include "browser_stack.h"
#include "include/cef_browser.h"
#include "../logging.h"
#include <algorithm>
#include <cstring>

// BrowserEntry implementation

void BrowserEntry::setCompositor(std::unique_ptr<Compositor> comp) {
    compositor = std::move(comp);
}

bool BrowserEntry::initCompositor(const CompositorContext& ctx, int width, int height) {
    // If compositor already set (e.g., via setCompositor for macOS pre-init), skip
    if (compositor) {
        return true;
    }
    compositor = std::make_unique<Compositor>();
#ifdef __APPLE__
    return compositor->init(ctx.window, width, height);
#else
    return compositor->init(ctx.gl_context, width, height);
#endif
}

void BrowserEntry::resize(int logical_w, int logical_h, int physical_w, int physical_h) {
    if (input_layer) {
        input_layer->setWindowSize(logical_w, logical_h);
    }

    if (resizeBrowser) {
        resizeBrowser(logical_w, logical_h, physical_w, physical_h);
    }

    compositor->resize(physical_w, physical_h);
}

void BrowserEntry::resize(int width, int height) {
    if (input_layer) {
        input_layer->setWindowSize(width, height);
    }

    if (resizeBrowser) {
        // Pass 0 for physical dimensions - client falls back to SDL query
        resizeBrowser(width, height, 0, 0);
    }
}

std::function<void(const void*, int, int)> BrowserEntry::makePaintCallback() {
    return [this](const void* buffer, int w, int h) {
        // Write to back buffer without blocking
        int write_idx = paint_write_idx.load(std::memory_order_relaxed);
        auto& buf = paint_buffers[write_idx];
        size_t size = static_cast<size_t>(w) * h * 4;
        if (buf.data.size() < size) {
            buf.data.resize(size);
        }
        std::memcpy(buf.data.data(), buffer, size);
        buf.width = w;
        buf.height = h;

        // Swap buffers (brief lock)
        {
            std::lock_guard<std::mutex> lock(paint_swap_mutex);
            buf.dirty = true;
            paint_write_idx.store(1 - write_idx, std::memory_order_release);
        }

        // Wake main loop to process the new frame
        if (wake_main_loop) {
            wake_main_loop();
        }
    };
}

void BrowserEntry::flushPaintBuffer() {
    std::lock_guard<std::mutex> lock(paint_swap_mutex);
    int read_idx = 1 - paint_write_idx.load(std::memory_order_acquire);
    auto& buf = paint_buffers[read_idx];
    if (buf.dirty && !buf.data.empty()) {
        compositor->updateOverlayPartial(buf.data.data(), buf.width, buf.height);
        buf.dirty = false;
    }
}

void BrowserEntry::importQueued() {
#ifdef __APPLE__
    compositor->importQueuedIOSurface();
#elif !defined(_WIN32)
    // Linux: import queued dmabuf
    compositor->importQueuedDmabuf();
#endif
    // Windows: no-op (no GPU texture import path)
}

void BrowserEntry::notifyScreenInfoChanged() {
    if (getBrowser) {
        if (auto browser = getBrowser()) {
            browser->GetHost()->NotifyScreenInfoChanged();
        }
    }
}

void BrowserEntry::forceRepaint() {
    if (getBrowser) {
        if (auto browser = getBrowser()) {
            browser->GetHost()->Invalidate(PET_VIEW);
        }
    }
}

// BrowserStack implementation

void BrowserStack::add(const std::string& name, std::unique_ptr<BrowserEntry> entry) {
    if (!entry) return;

    // Remove existing entry with same name
    remove(name);

    entry->name = name;
    BrowserEntry* ptr = entry.get();
    browsers_.push_back(std::move(entry));
    by_name_[name] = ptr;

    LOG_DEBUG(LOG_MAIN, "BrowserStack: added '%s' (total: %zu)", name.c_str(), browsers_.size());
}

void BrowserStack::remove(const std::string& name) {
    auto it = by_name_.find(name);
    if (it == by_name_.end()) return;

    BrowserEntry* ptr = it->second;
    by_name_.erase(it);

    // Find and remove from vector
    auto vec_it = std::find_if(browsers_.begin(), browsers_.end(),
        [ptr](const std::unique_ptr<BrowserEntry>& e) { return e.get() == ptr; });

    if (vec_it != browsers_.end()) {
        // Close the CEF browser before removing
        if ((*vec_it)->getBrowser) {
            if (auto browser = (*vec_it)->getBrowser()) {
                browser->GetHost()->CloseBrowser(true);
            }
        }
        browsers_.erase(vec_it);
    }

    LOG_DEBUG(LOG_MAIN, "BrowserStack: removed '%s' (total: %zu)", name.c_str(), browsers_.size());
}

BrowserEntry* BrowserStack::get(const std::string& name) {
    auto it = by_name_.find(name);
    return it != by_name_.end() ? it->second : nullptr;
}

const BrowserEntry* BrowserStack::get(const std::string& name) const {
    auto it = by_name_.find(name);
    return it != by_name_.end() ? it->second : nullptr;
}

BrowserLayer* BrowserStack::getInputLayer(const std::string& name) {
    auto* entry = get(name);
    return entry ? entry->input_layer.get() : nullptr;
}

PaintCallback BrowserStack::makePaintCallback(const std::string& name) {
    auto* entry = get(name);
    if (!entry) {
        return [](const void*, int, int) {};  // no-op
    }
    return entry->makePaintCallback();
}

void BrowserStack::flushAll() {
    for (auto& entry : browsers_) {
        entry->flushPaintBuffer();
    }
}

void BrowserStack::setAlpha(const std::string& name, float alpha) {
    auto* entry = get(name);
    if (entry) {
        entry->alpha = alpha;
    }
}

float BrowserStack::getAlpha(const std::string& name) const {
    auto* entry = get(name);
    return entry ? entry->alpha : 0.0f;
}

void BrowserStack::resizeAll(int logical_w, int logical_h, int physical_w, int physical_h) {
    for (auto& entry : browsers_) {
        entry->resize(logical_w, logical_h, physical_w, physical_h);
    }
}

void BrowserStack::resizeAll(int width, int height) {
    for (auto& entry : browsers_) {
        entry->resize(width, height);
    }
}

void BrowserStack::notifyAllScreenInfoChanged() {
    for (auto& entry : browsers_) {
        entry->notifyScreenInfoChanged();
    }
}

void BrowserStack::forceRepaintAll() {
    for (auto& entry : browsers_) {
        entry->forceRepaint();
    }
}

void BrowserStack::closeAllBrowsers() {
    for (auto& entry : browsers_) {
        if (entry->getBrowser) {
            if (auto browser = entry->getBrowser()) {
                browser->GetHost()->CloseBrowser(true);
            }
        }
    }
}

bool BrowserStack::allBrowsersClosed() const {
    for (const auto& entry : browsers_) {
        if (entry->isClosed && !entry->isClosed()) {
            return false;
        }
    }
    return true;
}

void BrowserStack::cleanupCompositors() {
    for (auto& entry : browsers_) {
        entry->compositor->cleanup();
    }
}

void BrowserStack::renderAll(int width, int height) {
    for (auto& entry : browsers_) {
        entry->flushPaintBuffer();
        entry->importQueued();
        if (entry->compositor->hasValidOverlay()) {
            entry->compositor->composite(width, height, entry->alpha);
        }
    }
}

bool BrowserStack::anyHasPendingContent() const {
    return false;
}
