#pragma once
#ifdef __APPLE__

#include <SDL3/SDL.h>
#include <cstdint>
#include <mutex>
#include <atomic>

// Forward declarations for ObjC types
#ifdef __OBJC__
@class CAMetalLayer;
@class NSView;
@class NSWindow;
#else
typedef void CAMetalLayer;
typedef void NSView;
typedef void NSWindow;
#endif

class MetalCompositor {
public:
    bool init(SDL_Window* window, uint32_t width, uint32_t height);
    void cleanup();

    // Update overlay with arbitrary size (recreates texture if needed)
    void updateOverlayPartial(const void* data, int src_width, int src_height);

    // Queue IOSurface for import on main thread (called from CEF thread)
    void queueIOSurface(void* ioSurface, int format, int width, int height);

    // Import queued IOSurface (called from main thread)
    bool importQueuedIOSurface();

    // Render frame
    void composite(uint32_t width, uint32_t height, float alpha);

    // Resize
    void resize(uint32_t width, uint32_t height);

    // Visibility
    void setVisible(bool visible);

    // Toggle transactional present (CA-synced for resize, async for normal rendering)
    void setPresentsWithTransaction(bool enabled);

    bool hasPendingImport() const { return iosurface_pending_.load(std::memory_order_acquire); }
    bool hasValidOverlay() const { return has_content_; }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // For video layer to position itself
    NSWindow* parentWindow() const { return parent_window_; }
    CAMetalLayer* layer() const { return metal_layer_; }

private:
    bool createPipeline();
    bool createTexture(uint32_t width, uint32_t height);

    SDL_Window* window_ = nullptr;
    NSWindow* parent_window_ = nullptr;
    NSView* metal_view_ = nullptr;
    CAMetalLayer* metal_layer_ = nullptr;

    void* device_ = nullptr;
    void* command_queue_ = nullptr;
    void* texture_ = nullptr;
    void* pipeline_state_ = nullptr;

    uint32_t width_ = 0;
    uint32_t height_ = 0;

    std::mutex mutex_;
    void* staging_buffer_ = nullptr;
    size_t staging_size_ = 0;
    std::atomic<bool> staging_dirty_{false};
    bool has_content_ = false;

    // IOSurface queue for zero-copy rendering
    struct QueuedIOSurface {
        void* surface = nullptr;  // IOSurfaceRef (retained)
        int format = 0;
        int width = 0;
        int height = 0;
    };
    QueuedIOSurface queued_iosurface_;
    std::atomic<bool> iosurface_pending_{false};
    uint32_t cached_surface_id_ = 0;   // To avoid recreating texture for same surface
    uint32_t cached_surface_seed_ = 0; // IOSurfaceGetSeed — skip compositing when content unchanged
};

#endif // __APPLE__
