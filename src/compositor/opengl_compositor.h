#pragma once

#ifdef __APPLE__
#include "context/cgl_context.h"
#include <OpenGL/gl3.h>
typedef CGLContext GLContext;
#elif defined(_WIN32)
#include "context/wgl_context.h"
#include <GL/gl.h>
#include <GL/glext.h>
typedef WGLContext GLContext;
#else
#include "context/egl_context.h"
typedef EGLContext_ GLContext;
#endif

#include <mutex>
#include <cstdint>
#include <atomic>

class OpenGLCompositor {
public:
    OpenGLCompositor();
    ~OpenGLCompositor();

    bool init(GLContext* ctx, uint32_t width, uint32_t height);
    void cleanup();

    // Update with partial/mismatched size data (copies overlapping region)
    void updateOverlayPartial(const void* data, int src_width, int src_height);

    // Get current compositor dimensions
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Composite overlay to screen with alpha blending
    void composite(uint32_t width, uint32_t height, float alpha);

    // Queue dmabuf for import (thread-safe, called from CEF callback)
    void queueDmabuf(int fd, uint32_t stride, uint64_t modifier, int width, int height);

    // Import queued dmabuf (must be called from main/GL thread)
    bool importQueuedDmabuf();

    // Resize resources
    void resize(uint32_t width, uint32_t height);

    // Set visibility (no-op on Linux, alpha controls rendering)
    void setVisible(bool visible) { (void)visible; }

    // Check if we have valid content to composite
    bool hasValidOverlay() const { return has_content_ && texture_valid_; }

private:
    bool createShader();

    GLContext* ctx_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // CEF texture - stores raw CEF frame at CEF's painted size (independent of viewport)
    GLuint cef_texture_ = 0;
    int cef_texture_width_ = 0;
    int cef_texture_height_ = 0;
    bool has_content_ = false;
    bool texture_valid_ = false;  // Set false on RECREATE, true when we get non-black frame

    // Thread safety
    std::mutex mutex_;

    // Shader program
    GLuint program_ = 0;
    GLint alpha_loc_ = -1;
    GLint swizzle_loc_ = -1;
    GLint tex_size_loc_ = -1;
    GLint view_size_loc_ = -1;
    GLint sampler_loc_ = -1;

    // VAO for fullscreen quad
    GLuint vao_ = 0;

    // Unique texture unit for this compositor (prevents interference between compositors)
    int texture_unit_ = 0;
    int log_count_ = 0;  // Per-instance log counter

#if !defined(__APPLE__) && !defined(_WIN32)
    // Dmabuf import (Linux only)
    GLuint dmabuf_texture_ = 0;
    void* egl_image_ = nullptr;  // EGLImage
    bool use_dmabuf_ = false;
    int dmabuf_width_ = 0;
    int dmabuf_height_ = 0;

    // Queued dmabuf for import on main thread
    struct QueuedDmabuf {
        int fd = -1;
        uint32_t stride = 0;
        uint64_t modifier = 0;
        int width = 0;
        int height = 0;
    };
    QueuedDmabuf queued_dmabuf_;
    std::atomic<bool> dmabuf_pending_{false};  // Fast-path check without mutex
#endif
};
