#pragma once

#include <wayland-client.h>
#include <atomic>
#include <mutex>

struct SDL_Window;
struct wp_viewporter;
struct wp_viewport;
struct zwp_linux_dmabuf_v1;

// Wayland subsurface for CEF browser content.
// Attaches CEF dmabufs directly as wl_buffers, bypassing EGL compositing.
// The Wayland compositor handles layering with the mpv video subsurface.
class WaylandBrowserSurface {
public:
    WaylandBrowserSurface();
    ~WaylandBrowserSurface();

    // Initialize: create subsurface above parent (SDL window surface)
    bool init(SDL_Window* window);
    void cleanup();

    // Thread-safe: queue dmabuf from CEF's OnAcceleratedPaint callback
    void queueDmabuf(int fd, uint32_t stride, uint64_t modifier, int width, int height);

    // Main thread: create wl_buffer from queued dmabuf, attach+commit
    bool commitQueued();

    void setDestinationSize(int width, int height);
    void hide();

    bool isInitialized() const { return subsurface_ != nullptr; }
    wl_surface* surface() const { return surface_; }

    // Public for Wayland listener structs
    static void registryGlobal(void* data, wl_registry* registry,
                               uint32_t name, const char* interface, uint32_t version);
    static void registryGlobalRemove(void*, wl_registry*, uint32_t) {}
    static void bufferRelease(void* data, wl_buffer* buffer);

private:
    bool initWayland(SDL_Window* window);
    bool createSubsurface(wl_surface* parent);

    wl_display* wl_display_ = nullptr;
    wl_compositor* wl_compositor_ = nullptr;
    wl_subcompositor* wl_subcompositor_ = nullptr;
    wl_surface* surface_ = nullptr;
    wl_subsurface* subsurface_ = nullptr;
    wp_viewporter* viewporter_ = nullptr;
    wp_viewport* viewport_ = nullptr;
    zwp_linux_dmabuf_v1* dmabuf_manager_ = nullptr;

    // Queued dmabuf from CEF thread
    struct DmabufFrame {
        int fd = -1;
        uint32_t stride = 0;
        uint64_t modifier = 0;
        int width = 0;
        int height = 0;
    };
    DmabufFrame queued_;
    std::mutex queue_mutex_;
    std::atomic<bool> pending_{false};

    // Active buffer tracking for release
    wl_buffer* current_buffer_ = nullptr;
    int current_fd_ = -1;
    wl_buffer* prev_buffer_ = nullptr;
    int prev_fd_ = -1;
};
