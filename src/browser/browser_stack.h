#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <atomic>
#include <mutex>
#include <memory>
#include <functional>
#include <cstdint>

#include "include/cef_client.h"
#include "../input/browser_layer.h"

#ifdef __APPLE__
#include "../compositor/metal_compositor.h"
using Compositor = MetalCompositor;
#else
#include "../compositor/opengl_compositor.h"
using Compositor = OpenGLCompositor;
#endif

// Forward declarations for graphics contexts
struct SDL_Window;
#ifndef __APPLE__
class EGLContext_;
class WGLContext;
using GLContext =
#ifdef _WIN32
    WGLContext;
#else
    EGLContext_;
#endif
#endif

// Unified context for compositor initialization (platform picks what it needs)
struct CompositorContext {
    SDL_Window* window = nullptr;  // macOS uses this
#ifndef __APPLE__
    GLContext* gl_context = nullptr;  // Windows/Linux use this
#endif
};

// Paint buffer for double-buffered CEF paint callbacks
struct PaintBuffer {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    bool dirty = false;
};

// Per-browser state container
struct BrowserEntry {
    std::string name;
    CefRefPtr<CefClient> client;
    std::function<CefRefPtr<CefBrowser>()> getBrowser;  // set at creation, returns browser when available
    std::function<void(int, int, int, int)> resizeBrowser;  // set at creation, resizes the CEF browser (logical_w, logical_h, physical_w, physical_h)
    std::function<InputReceiver*()> getInputReceiver;  // set at creation, returns input receiver
    std::function<bool()> isClosed;  // set at creation, returns true when browser is closed
    std::unique_ptr<BrowserLayer> input_layer;
    std::array<PaintBuffer, 2> paint_buffers;
    std::atomic<int> paint_write_idx{0};
    std::mutex paint_swap_mutex;
    std::unique_ptr<Compositor> compositor;  // owned
    float alpha = 1.0f;
    std::function<void()> wake_main_loop;  // Called after paint to wake main loop

    // Set a pre-created compositor (for macOS pre-init optimization)
    void setCompositor(std::unique_ptr<Compositor> comp);

    // Initialize the compositor (call once after construction, no-op if already set)
    bool initCompositor(const CompositorContext& ctx, int width, int height);

    // Resize the browser, input layer, and compositor
    // logical_w/h for browser, physical_w/h for compositor (HiDPI)
    void resize(int logical_w, int logical_h, int physical_w, int physical_h);

    // Legacy resize (logical only, no compositor resize)
    void resize(int width, int height);

    // Create paint callback for CEF
    std::function<void(const void*, int, int)> makePaintCallback();

    // Flush dirty paint buffer to compositor
    void flushPaintBuffer();

    // Platform-specific compositor operations
    void importQueued();   // importQueuedIOSurface (macOS) / importQueuedDmabuf (Linux)
    void flushOverlay();   // OpenGL texture upload (Windows/Linux only)

    // Notify browser of screen info change (HiDPI scale change)
    void notifyScreenInfoChanged();

    // Force browser repaint (during resize)
    void forceRepaint();
};

// Callback type for paint events
using PaintCallback = std::function<void(const void* buffer, int width, int height)>;

// Manages all browsers in z-order (back to front)
class BrowserStack {
public:
    // Lifecycle
    void add(const std::string& name, std::unique_ptr<BrowserEntry> entry);
    void remove(const std::string& name);
    BrowserEntry* get(const std::string& name);
    const BrowserEntry* get(const std::string& name) const;

    // Input layer access (caller manages InputStack separately)
    BrowserLayer* getInputLayer(const std::string& name);

    // Paint management
    PaintCallback makePaintCallback(const std::string& name);
    void flushAll();  // flush all dirty paint buffers to compositors

    // Visibility (for fade animation before removal)
    void setAlpha(const std::string& name, float alpha);
    float getAlpha(const std::string& name) const;

    // Resize all browsers and compositors
    // logical_w/h for browsers, physical_w/h for compositors (HiDPI)
    void resizeAll(int logical_w, int logical_h, int physical_w, int physical_h);

    // Legacy resize (logical only, no compositor resize)
    void resizeAll(int width, int height);

    // Notify all browsers of screen info change
    void notifyAllScreenInfoChanged();

    // Force all browsers to repaint
    void forceRepaintAll();

    // Close all browsers (call before CEF shutdown)
    void closeAllBrowsers();

    // Check if all browsers have finished closing
    bool allBrowsersClosed() const;

    // Cleanup all compositors (call before destroying graphics context)
    void cleanupCompositors();

    // Flush paint buffers, import GPU textures, and composite all visible browsers
    void renderAll(int width, int height);

    // Check if stack is empty
    bool empty() const { return browsers_.empty(); }

    // Size
    size_t size() const { return browsers_.size(); }

    // Check if any browser has pending content to render
    bool anyHasPendingContent() const;

private:
    std::vector<std::unique_ptr<BrowserEntry>> browsers_;  // z-order: back to front
    std::unordered_map<std::string, BrowserEntry*> by_name_;
};
