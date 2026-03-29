#include "opengl_renderer.h"
#include "mpv/mpv_player.h"
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include "logging.h"

#ifdef _WIN32
#include "context/wgl_context.h"
#else
#include "context/egl_context.h"
#endif

// Shader for compositing video texture (fullscreen triangle)
#ifdef _WIN32
static const char* composite_vert = R"(#version 330 core
out vec2 vTexCoord;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vTexCoord = pos;
    vTexCoord.y = 1.0 - vTexCoord.y;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* composite_frag = R"(#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D videoTex;
void main() {
    fragColor = texture(videoTex, vTexCoord);
}
)";
#else
static const char* composite_vert = R"(#version 300 es
out vec2 vTexCoord;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vTexCoord = pos;
    vTexCoord.y = 1.0 - vTexCoord.y;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* composite_frag = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D videoTex;
void main() {
    fragColor = texture(videoTex, vTexCoord);
}
)";
#endif

OpenGLRenderer::OpenGLRenderer(MpvPlayer* player) : player_(player) {}

OpenGLRenderer::~OpenGLRenderer() {
    OpenGLRenderer::cleanup();
}

#ifdef _WIN32
bool OpenGLRenderer::initThreaded(WGLContext* wgl) {
    wgl_ = wgl;
    shared_ctx_ = wgl->createSharedContext();
    if (!shared_ctx_) {
        LOG_ERROR(LOG_VIDEO, "Failed to create shared WGL context for video");
        return false;
    }
    threaded_ = true;
    LOG_INFO(LOG_VIDEO, "OpenGLRenderer initialized for threaded rendering (WGL)");
    return true;
}
#else
bool OpenGLRenderer::initThreaded(EGLContext_* egl) {
    egl_ = egl;
    shared_ctx_ = egl->createSharedContext();
    if (shared_ctx_ == EGL_NO_CONTEXT) {
        LOG_ERROR(LOG_VIDEO, "Failed to create shared EGL context for video");
        return false;
    }
    threaded_ = true;
    LOG_INFO(LOG_VIDEO, "OpenGLRenderer initialized for threaded rendering (EGL)");
    return true;
}
#endif

bool OpenGLRenderer::hasFrame() const {
    return player_->hasFrame();
}

void OpenGLRenderer::createFBO(int width, int height) {
    if (buffers_[0].fbo && fbo_width_ == width && fbo_height_ == height) {
        return;  // Already have correct size
    }

    destroyFBO();

    // Create double-buffered FBOs
    for (int i = 0; i < NUM_BUFFERS; i++) {
        auto& buf = buffers_[i];

        glGenFramebuffers(1, &buf.fbo);
        glGenTextures(1, &buf.texture);
        glGenRenderbuffers(1, &buf.depth_rb);

        glBindTexture(GL_TEXTURE_2D, buf.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindRenderbuffer(GL_RENDERBUFFER, buf.depth_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);

        glBindFramebuffer(GL_FRAMEBUFFER, buf.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buf.texture, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, buf.depth_rb);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR(LOG_VIDEO, "FBO %d incomplete: 0x%x", i, status);
            destroyFBO();
            return;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fbo_width_ = width;
    fbo_height_ = height;
    write_index_ = 0;
    LOG_INFO(LOG_VIDEO, "Created double-buffered video FBOs: %dx%d", width, height);
}

void OpenGLRenderer::destroyFBO() {
    front_texture_.store(0);  // Clear atomic first

    for (int i = 0; i < NUM_BUFFERS; i++) {
        auto& buf = buffers_[i];
        if (buf.fbo) {
            glDeleteFramebuffers(1, &buf.fbo);
            buf.fbo = 0;
        }
        if (buf.texture) {
            glDeleteTextures(1, &buf.texture);
            buf.texture = 0;
        }
        if (buf.depth_rb) {
            glDeleteRenderbuffers(1, &buf.depth_rb);
            buf.depth_rb = 0;
        }
    }
    fbo_width_ = 0;
    fbo_height_ = 0;
}

void OpenGLRenderer::renderToFBO(int fbo, int width, int height, bool flip) {
    mpv_opengl_fbo fbo_params{};
    fbo_params.fbo = fbo;
    fbo_params.w = width;
    fbo_params.h = height;
    fbo_params.internal_format = 0;  // Let mpv decide

    int flip_y = flip ? 1 : 0;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo_params},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    static bool first = true;
    if (first) {
        LOG_INFO(LOG_MPV, "render: %dx%d fbo=%d flip=%d", width, height, fbo, flip ? 1 : 0);
        first = false;
    }

    mpv_render_context_render(player_->renderContext(), params);
}

bool OpenGLRenderer::render(int width, int height) {
    if (threaded_) {
        // Make shared context current on this thread
#ifdef _WIN32
        if (!wgl_->makeCurrent(shared_ctx_)) {
#else
        if (!egl_->makeCurrent(shared_ctx_)) {
#endif
            LOG_ERROR(LOG_VIDEO, "Failed to make shared context current");
            return false;
        }

        // Create/resize FBOs if needed (brief lock)
        {
            std::lock_guard<std::mutex> lock(fbo_mutex_);
            createFBO(width, height);
            if (!buffers_[0].fbo) {
                LOG_ERROR(LOG_VIDEO, "FBO creation failed");
#ifdef _WIN32
                wgl_->makeCurrent(nullptr);
#else
                egl_->makeCurrent(EGL_NO_CONTEXT);
#endif
                return false;
            }
        }

        // Render to back buffer
        auto& back = buffers_[write_index_];
        glBindFramebuffer(GL_FRAMEBUFFER, back.fbo);
        glViewport(0, 0, width, height);
        renderToFBO(back.fbo, width, height, false);  // No flip - FBO is top-down

        // Wait for render to complete before publishing texture
        glFinish();

        // Publish this texture as front and swap to other buffer for next frame
        front_texture_.store(back.texture);
        write_index_ = (write_index_ + 1) % NUM_BUFFERS;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
#ifdef _WIN32
        wgl_->makeCurrent(nullptr);
#else
        egl_->makeCurrent(EGL_NO_CONTEXT);
#endif
        has_rendered_.store(true);
    } else {
        // Direct rendering to default framebuffer
        renderToFBO(0, width, height, true);  // Flip for screen
        has_rendered_.store(true);
    }
    return true;
}

void OpenGLRenderer::composite(int width, int height) {
    if (!threaded_ || !has_rendered_.load()) {
        return;
    }

    // Create composite shader if needed
    if (!composite_program_) {
        GLuint vert = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vert, 1, &composite_vert, nullptr);
        glCompileShader(vert);

        GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(frag, 1, &composite_frag, nullptr);
        glCompileShader(frag);

        composite_program_ = glCreateProgram();
        glAttachShader(composite_program_, vert);
        glAttachShader(composite_program_, frag);
        glLinkProgram(composite_program_);

        glDeleteShader(vert);
        glDeleteShader(frag);

        glGenVertexArrays(1, &composite_vao_);
        composite_tex_loc_ = glGetUniformLocation(composite_program_, "videoTex");
    }

    // Use atomically published front texture
    GLuint tex = front_texture_.load();
    if (!tex) return;

    glUseProgram(composite_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(composite_tex_loc_, 0);

    glBindVertexArray(composite_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
}

void OpenGLRenderer::resize(int width, int height) {
    // FBOs will be recreated on next render if size changed
    (void)width;
    (void)height;
}

void OpenGLRenderer::cleanup() {
#ifdef _WIN32
    if (threaded_ && wgl_) {
        wgl_->makeCurrent(shared_ctx_);
    }
#else
    if (threaded_ && egl_) {
        egl_->makeCurrent(shared_ctx_);
    }
#endif

    destroyFBO();

    if (composite_program_) {
        glDeleteProgram(composite_program_);
        composite_program_ = 0;
    }
    if (composite_vao_) {
        glDeleteVertexArrays(1, &composite_vao_);
        composite_vao_ = 0;
    }

#ifdef _WIN32
    if (threaded_ && wgl_) {
        wgl_->makeCurrent(nullptr);
        wgl_->destroyContext(shared_ctx_);
        shared_ctx_ = nullptr;
    }
#else
    if (threaded_ && egl_) {
        egl_->makeCurrent(EGL_NO_CONTEXT);
        egl_->destroyContext(shared_ctx_);
        shared_ctx_ = EGL_NO_CONTEXT;
    }
#endif

    threaded_ = false;
    has_rendered_.store(false);
}

float OpenGLRenderer::getClearAlpha(bool video_ready) const {
    // For threaded mode, use transparent clear so video shows through
    // For sync mode, video renders first so use opaque clear
    if (threaded_) {
        return video_ready ? 0.0f : 1.0f;
    }
    return 1.0f;
}
