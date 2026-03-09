#pragma once
#include "video_renderer.h"

#ifdef _WIN32
#include "context/gl_loader.h"
#else
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#endif

#include <atomic>
#include <mutex>

class MpvPlayer;
class EGLContext_;
class WGLContext;

class OpenGLRenderer : public VideoRenderer {
public:
    explicit OpenGLRenderer(MpvPlayer* player);
    ~OpenGLRenderer();

    // Initialize for threaded rendering (creates shared context + FBO)
#ifdef _WIN32
    bool initThreaded(WGLContext* wgl);
#else
    bool initThreaded(EGLContext_* egl);
#endif

    bool hasFrame() const override;
    bool render(int width, int height) override;
    void composite(int width, int height) override;

    void setVisible(bool) override {}
    void resize(int width, int height) override;
    void setDestinationSize(int, int) override {}
    void setColorspace() override {}
    void cleanup() override;
    float getClearAlpha(bool video_ready) const override;
    bool isHdr() const override { return false; }

private:
    void createFBO(int width, int height);
    void destroyFBO();
    void renderToFBO(int fbo, int width, int height, bool flip);

    MpvPlayer* player_;
    bool threaded_ = false;

#ifdef _WIN32
    WGLContext* wgl_ = nullptr;
    HGLRC shared_ctx_ = nullptr;
#else
    EGLContext_* egl_ = nullptr;
    EGLContext shared_ctx_ = EGL_NO_CONTEXT;
#endif

    // Double-buffered FBOs for lock-free rendering
    static constexpr int NUM_BUFFERS = 2;
    struct FBOBuffer {
        GLuint fbo = 0;
        GLuint texture = 0;
        GLuint depth_rb = 0;
    };
    FBOBuffer buffers_[NUM_BUFFERS];
    int write_index_ = 0;  // Render thread writes here
    int fbo_width_ = 0;
    int fbo_height_ = 0;

    GLuint composite_program_ = 0;
    GLuint composite_vao_ = 0;
    GLint composite_tex_loc_ = -1;

    std::mutex fbo_mutex_;
    std::atomic<bool> has_rendered_{false};
    std::atomic<GLuint> front_texture_{0};  // Main thread reads this
};
