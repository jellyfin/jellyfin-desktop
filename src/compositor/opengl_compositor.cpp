#include "compositor/opengl_compositor.h"
#include "logging.h"

#if !defined(__APPLE__) && !defined(_WIN32)
#include <drm_fourcc.h>  // For DRM_FORMAT_ARGB8888
#include <unistd.h>      // For close()
// EGL function pointers for dmabuf import
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
#endif

#ifdef __APPLE__
// macOS gl3.h defines GL_BGRA, not GL_BGRA_EXT
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT GL_BGRA
#endif
#elif defined(_WIN32)
// Windows: Load OpenGL extension function pointers
static PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
static PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
static PFNGLBUFFERDATAPROC glBufferData = nullptr;
static PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;
static PFNGLMAPBUFFERRANGEPROC glMapBufferRange = nullptr;
static PFNGLUNMAPBUFFERPROC glUnmapBuffer = nullptr;
static PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = nullptr;
static PFNGLBINDVERTEXARRAYPROC glBindVertexArray = nullptr;
static PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = nullptr;
static PFNGLCREATESHADERPROC glCreateShader = nullptr;
static PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
static PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
static PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
static PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
static PFNGLDELETESHADERPROC glDeleteShader = nullptr;
static PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
static PFNGLATTACHSHADERPROC glAttachShader = nullptr;
static PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
static PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
static PFNGLDELETEPROGRAMPROC glDeleteProgram = nullptr;
static PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
static PFNGLUNIFORM1FPROC glUniform1f = nullptr;
static PFNGLUNIFORM2FPROC glUniform2f = nullptr;
static PFNGLUNIFORM1IPROC glUniform1i = nullptr;
static PFNGLACTIVETEXTUREPROC glActiveTexture = nullptr;

static bool s_wglExtensionsLoaded = false;

static void loadWGLExtensions() {
    if (s_wglExtensionsLoaded) return;
    glGenBuffers = (PFNGLGENBUFFERSPROC)wglGetProcAddress("glGenBuffers");
    glBindBuffer = (PFNGLBINDBUFFERPROC)wglGetProcAddress("glBindBuffer");
    glBufferData = (PFNGLBUFFERDATAPROC)wglGetProcAddress("glBufferData");
    glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)wglGetProcAddress("glDeleteBuffers");
    glMapBufferRange = (PFNGLMAPBUFFERRANGEPROC)wglGetProcAddress("glMapBufferRange");
    glUnmapBuffer = (PFNGLUNMAPBUFFERPROC)wglGetProcAddress("glUnmapBuffer");
    glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)wglGetProcAddress("glGenVertexArrays");
    glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)wglGetProcAddress("glBindVertexArray");
    glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)wglGetProcAddress("glDeleteVertexArrays");
    glCreateShader = (PFNGLCREATESHADERPROC)wglGetProcAddress("glCreateShader");
    glShaderSource = (PFNGLSHADERSOURCEPROC)wglGetProcAddress("glShaderSource");
    glCompileShader = (PFNGLCOMPILESHADERPROC)wglGetProcAddress("glCompileShader");
    glGetShaderiv = (PFNGLGETSHADERIVPROC)wglGetProcAddress("glGetShaderiv");
    glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)wglGetProcAddress("glGetShaderInfoLog");
    glDeleteShader = (PFNGLDELETESHADERPROC)wglGetProcAddress("glDeleteShader");
    glCreateProgram = (PFNGLCREATEPROGRAMPROC)wglGetProcAddress("glCreateProgram");
    glAttachShader = (PFNGLATTACHSHADERPROC)wglGetProcAddress("glAttachShader");
    glLinkProgram = (PFNGLLINKPROGRAMPROC)wglGetProcAddress("glLinkProgram");
    glGetProgramiv = (PFNGLGETPROGRAMIVPROC)wglGetProcAddress("glGetProgramiv");
    glGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)wglGetProcAddress("glGetProgramInfoLog");
    glDeleteProgram = (PFNGLDELETEPROGRAMPROC)wglGetProcAddress("glDeleteProgram");
    glUseProgram = (PFNGLUSEPROGRAMPROC)wglGetProcAddress("glUseProgram");
    glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)wglGetProcAddress("glGetUniformLocation");
    glUniform1f = (PFNGLUNIFORM1FPROC)wglGetProcAddress("glUniform1f");
    glUniform2f = (PFNGLUNIFORM2FPROC)wglGetProcAddress("glUniform2f");
    glUniform1i = (PFNGLUNIFORM1IPROC)wglGetProcAddress("glUniform1i");
    glActiveTexture = (PFNGLACTIVETEXTUREPROC)wglGetProcAddress("glActiveTexture");
    s_wglExtensionsLoaded = true;
}
#endif

#ifdef __APPLE__
// macOS: Desktop OpenGL 3.2 Core with GL_TEXTURE_2D (software path)
static const char* vert_src = R"(#version 150
out vec2 texCoord;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texCoord = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* frag_src = R"(#version 150
in vec2 texCoord;
out vec4 fragColor;
uniform sampler2D overlayTex;
uniform float alpha;
void main() {
    vec4 color = texture(overlayTex, texCoord);
    fragColor = color * alpha;
}
)";
#elif defined(_WIN32)
// Windows: Desktop OpenGL 2.1+ with GL_TEXTURE_2D
static const char* vert_src = R"(#version 130
out vec2 texCoord;
uniform float flipY;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texCoord = vec2(pos.x, mix(pos.y, 1.0 - pos.y, flipY));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* frag_src = R"(#version 130
in vec2 texCoord;
out vec4 fragColor;
uniform sampler2D overlayTex;
uniform float alpha;
uniform float swizzleBgra;
void main() {
    vec4 color = texture(overlayTex, texCoord);
    // CEF provides BGRA - swizzle to RGBA when rendering to default framebuffer,
    // skip swizzle when rendering to D3D11 interop FBO (already B8G8R8A8)
    vec4 swizzled = mix(color, color.bgra, swizzleBgra);
    fragColor = swizzled * alpha;
}
)";
#else
// Linux: OpenGL ES 3.0
// Render CEF texture at 1:1 pixels using gl_FragCoord
static const char* vert_src = R"(#version 300 es
void main() {
    // Fullscreen triangle
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* frag_src = R"(#version 300 es
precision mediump float;
out vec4 fragColor;
uniform sampler2D overlayTex;
uniform float alpha;
uniform float swizzleBgra;
uniform vec2 texSize;
uniform vec2 viewSize;
void main() {
    int px = int(gl_FragCoord.x);
    // Flip Y using viewport height so texture anchors to TOP
    int tex_y = int(viewSize.y) - 1 - int(gl_FragCoord.y);

    // Out of bounds = transparent (let background show through)
    if (px < 0 || tex_y < 0 || px >= int(texSize.x) || tex_y >= int(texSize.y)) {
        discard;
    }

    vec4 color = texelFetch(overlayTex, ivec2(px, tex_y), 0);
    // Software path provides BGRA, dmabuf provides RGBA (driver converts)
    if (swizzleBgra > 0.5) {
        color = color.bgra;
    }
    fragColor = color * alpha;
}
)";
#endif

OpenGLCompositor::OpenGLCompositor() = default;

OpenGLCompositor::~OpenGLCompositor() {
    cleanup();
}

bool OpenGLCompositor::init(GLContext* ctx, uint32_t width, uint32_t height) {
    ctx_ = ctx;
    width_ = width;
    height_ = height;

    // Assign unique texture unit to this compositor instance
    static int next_texture_unit = 0;
    texture_unit_ = next_texture_unit++;
    LOG_INFO(LOG_COMPOSITOR, "Compositor initialized with texture unit %d", texture_unit_);

#ifdef _WIN32
    // Ensure WGL context is current - it may have been displaced by D3D11/Vulkan
    // device creation in VideoStack::create() before compositor init
    ctx_->makeCurrent();
    loadWGLExtensions();
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
    // Load EGL extensions for dmabuf import
    if (!glEGLImageTargetTexture2DOES) {
        glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");
    }
    if (!eglCreateImageKHR) {
        eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
            eglGetProcAddress("eglCreateImageKHR");
    }
    if (!eglDestroyImageKHR) {
        eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
            eglGetProcAddress("eglDestroyImageKHR");
    }
    if (glEGLImageTargetTexture2DOES && eglCreateImageKHR && eglDestroyImageKHR) {
        LOG_INFO(LOG_COMPOSITOR, "EGL dmabuf import extensions loaded");
    } else {
        LOG_WARN(LOG_COMPOSITOR, "EGL dmabuf import extensions not available");
    }
#endif

    if (!createShader()) return false;

    // Create VAO (required for GLES 3.0 / OpenGL core)
    glGenVertexArrays(1, &vao_);

    return true;
}

bool OpenGLCompositor::createShader() {
    // Compile vertex shader
    GLuint vert = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vert, 1, &vert_src, nullptr);
    glCompileShader(vert);

    GLint status;
    glGetShaderiv(vert, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(vert, 512, nullptr, log);
        LOG_ERROR(LOG_COMPOSITOR, "Vertex shader error: %s", log);
        glDeleteShader(vert);
        return false;
    }

    // Compile fragment shader
    GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(frag, 1, &frag_src, nullptr);
    glCompileShader(frag);

    glGetShaderiv(frag, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(frag, 512, nullptr, log);
        LOG_ERROR(LOG_COMPOSITOR, "Fragment shader error: %s", log);
        glDeleteShader(vert);
        glDeleteShader(frag);
        return false;
    }

    // Link program
    program_ = glCreateProgram();
    glAttachShader(program_, vert);
    glAttachShader(program_, frag);
    glLinkProgram(program_);

    glGetProgramiv(program_, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(program_, 512, nullptr, log);
        LOG_ERROR(LOG_COMPOSITOR, "Program link error: %s", log);
        glDeleteShader(vert);
        glDeleteShader(frag);
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    // Get uniform locations
    alpha_loc_ = glGetUniformLocation(program_, "alpha");
    swizzle_loc_ = glGetUniformLocation(program_, "swizzleBgra");
    flip_y_loc_ = glGetUniformLocation(program_, "flipY");
    tex_size_loc_ = glGetUniformLocation(program_, "texSize");
    view_size_loc_ = glGetUniformLocation(program_, "viewSize");
    sampler_loc_ = glGetUniformLocation(program_, "overlayTex");

    return true;
}

void OpenGLCompositor::updateOverlayPartial(const void* data, int src_width, int src_height) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!data || src_width <= 0 || src_height <= 0) return;

    // Recreate CEF texture if size changed or texture doesn't exist
    if (cef_texture_ == 0 || src_width != cef_texture_width_ || src_height != cef_texture_height_) {
        LOG_DEBUG(LOG_COMPOSITOR, "updateOverlayPartial: RECREATE %dx%d -> %dx%d (viewport=%ux%u)",
                  cef_texture_width_, cef_texture_height_, src_width, src_height, width_, height_);
        if (cef_texture_) {
            glDeleteTextures(1, &cef_texture_);
        }
        glGenTextures(1, &cef_texture_);
        glBindTexture(GL_TEXTURE_2D, cef_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);  // No interpolation for 1:1
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Reset pixel unpack state before texture allocation
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, src_width, src_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        cef_texture_width_ = src_width;
        cef_texture_height_ = src_height;
        texture_valid_ = false;  // Need valid data before rendering
        LOG_DEBUG(LOG_COMPOSITOR, "Created CEF texture %dx%d", src_width, src_height);
    }

    // Upload CEF frame directly to texture
    // Reset pixel unpack state to ensure no offset/stride issues
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glBindTexture(GL_TEXTURE_2D, cef_texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_width, src_height, GL_RGBA, GL_UNSIGNED_BYTE, data);
    texture_valid_ = true;
    has_content_ = true;
}

void OpenGLCompositor::queueDmabuf(int fd, uint32_t stride, uint64_t modifier, int w, int h) {
#if !defined(__APPLE__) && !defined(_WIN32)
    std::lock_guard<std::mutex> lock(mutex_);

    // Close any previously queued fd that wasn't imported
    if (dmabuf_pending_.load(std::memory_order_relaxed) && queued_dmabuf_.fd >= 0) {
        close(queued_dmabuf_.fd);
    }

    queued_dmabuf_.fd = fd;
    queued_dmabuf_.stride = stride;
    queued_dmabuf_.modifier = modifier;
    queued_dmabuf_.width = w;
    queued_dmabuf_.height = h;
    dmabuf_pending_.store(true, std::memory_order_release);
#else
    (void)fd; (void)stride; (void)modifier; (void)w; (void)h;
#endif
}

bool OpenGLCompositor::importQueuedDmabuf() {
#if !defined(__APPLE__) && !defined(_WIN32)
    // Fast-path: check atomic without lock
    if (!dmabuf_pending_.load(std::memory_order_acquire)) {
        return false;
    }

    if (!glEGLImageTargetTexture2DOES || !eglCreateImageKHR || !eglDestroyImageKHR || !ctx_) {
        return false;
    }

    // Get queued dmabuf under lock
    int fd;
    uint32_t stride;
    uint64_t modifier;
    int w, h;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dmabuf_pending_.load(std::memory_order_relaxed)) {
            return false;  // Another thread got it
        }
        fd = queued_dmabuf_.fd;
        stride = queued_dmabuf_.stride;
        modifier = queued_dmabuf_.modifier;
        w = queued_dmabuf_.width;
        h = queued_dmabuf_.height;
        dmabuf_pending_.store(false, std::memory_order_relaxed);
        queued_dmabuf_.fd = -1;
    }

    if (fd < 0) {
        return false;
    }

    EGLDisplay display = ctx_->display();

    // Destroy previous EGLImage if exists
    if (egl_image_) {
        eglDestroyImageKHR(display, static_cast<EGLImageKHR>(egl_image_));
        egl_image_ = nullptr;
    }

    // Create dmabuf texture if needed
    if (!dmabuf_texture_) {
        glGenTextures(1, &dmabuf_texture_);
        glBindTexture(GL_TEXTURE_2D, dmabuf_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Create EGLImage from dmabuf
    // CEF uses DRM_FORMAT_ARGB8888 - EGL import handles format conversion
    // DRM_FORMAT_MOD_INVALID means no modifier - don't include modifier attrs
    EGLImageKHR image;
    if (modifier == DRM_FORMAT_MOD_INVALID) {
        EGLint attrs[] = {
            EGL_WIDTH, w,
            EGL_HEIGHT, h,
            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(stride),
            EGL_NONE
        };
        image = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    } else {
        EGLint attrs[] = {
            EGL_WIDTH, w,
            EGL_HEIGHT, h,
            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(stride),
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(modifier & 0xFFFFFFFF),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>(modifier >> 32),
            EGL_NONE
        };
        image = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    }

    // Close the fd after creating the EGLImage (EGL keeps its own reference)
    close(fd);

    if (!image) {
        EGLint err = eglGetError();
        LOG_ERROR(LOG_COMPOSITOR, "eglCreateImageKHR failed: 0x%x", err);
        return false;
    }
    egl_image_ = image;

    // Bind to texture
    glBindTexture(GL_TEXTURE_2D, dmabuf_texture_);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

    GLenum glErr = glGetError();
    if (glErr != GL_NO_ERROR) {
        LOG_ERROR(LOG_COMPOSITOR, "glEGLImageTargetTexture2DOES failed: 0x%x", glErr);
        eglDestroyImageKHR(display, image);
        egl_image_ = nullptr;
        return false;
    }

    dmabuf_width_ = w;
    dmabuf_height_ = h;
    use_dmabuf_ = true;
    has_content_ = true;
    texture_valid_ = true;

    static bool first = true;
    if (first) {
        LOG_INFO(LOG_COMPOSITOR, "dmabuf imported: %dx%d stride=%u modifier=0x%lx",
                 w, h, stride, modifier);
        first = false;
    }

    return true;
#else
    return false;
#endif
}

void OpenGLCompositor::composite(uint32_t width, uint32_t height, float alpha) {
    if (!has_content_ || !program_) {
        return;
    }

    glViewport(0, 0, width, height);

    // Enable blending with premultiplied alpha
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(program_);
    glUniform1f(alpha_loc_, alpha);
    if (view_size_loc_ >= 0) glUniform2f(view_size_loc_, static_cast<float>(width), static_cast<float>(height));

#ifdef _WIN32
    // DComp overlay FBO: no Y-flip (D3D11 top-left origin)
    // Default framebuffer: flip Y
    // BGRA swizzle always needed — GL driver handles byte-order mapping internally
    if (flip_y_loc_ >= 0) glUniform1f(flip_y_loc_, dcomp_overlay_ ? 0.0f : 1.0f);
    if (swizzle_loc_ >= 0) glUniform1f(swizzle_loc_, 1.0f);
#endif

    // Use this compositor's dedicated texture unit to prevent interference
    glActiveTexture(GL_TEXTURE0 + texture_unit_);
    if (sampler_loc_ >= 0) glUniform1i(sampler_loc_, texture_unit_);

#if !defined(__APPLE__) && !defined(_WIN32)
    // Bind texture and get dimensions under lock to prevent race with updateOverlayPartial
    GLuint tex_to_use = 0;
    int tex_w = 0, tex_h = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (use_dmabuf_ && dmabuf_texture_) {
            tex_to_use = dmabuf_texture_;
            tex_w = dmabuf_width_;
            tex_h = dmabuf_height_;
            glBindTexture(GL_TEXTURE_2D, tex_to_use);
            if (swizzle_loc_ >= 0) glUniform1f(swizzle_loc_, 0.0f);
            if (log_count_++ < 3) LOG_INFO(LOG_COMPOSITOR, "composite: DMABUF tex=%u size=%dx%d view=%ux%u", tex_to_use, tex_w, tex_h, width, height);
        } else if (cef_texture_) {
            tex_to_use = cef_texture_;
            tex_w = cef_texture_width_;
            tex_h = cef_texture_height_;
            glBindTexture(GL_TEXTURE_2D, tex_to_use);
            if (swizzle_loc_ >= 0) glUniform1f(swizzle_loc_, 1.0f);
        }
    }
    if (!tex_to_use) return;
    if (tex_size_loc_ >= 0) glUniform2f(tex_size_loc_, static_cast<float>(tex_w), static_cast<float>(tex_h));
#else
    if (cef_texture_) {
        glBindTexture(GL_TEXTURE_2D, cef_texture_);
        if (tex_size_loc_ >= 0) glUniform2f(tex_size_loc_, static_cast<float>(cef_texture_width_), static_cast<float>(cef_texture_height_));
    } else {
        return;
    }
#endif

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
}

void OpenGLCompositor::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || (width == width_ && height == height_)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    width_ = width;
    height_ = height;
}

void OpenGLCompositor::cleanup() {
    if (!ctx_) return;

#if !defined(__APPLE__) && !defined(_WIN32)
    // Clean up dmabuf resources
    if (egl_image_ && eglDestroyImageKHR) {
        eglDestroyImageKHR(ctx_->display(), static_cast<EGLImageKHR>(egl_image_));
        egl_image_ = nullptr;
    }
    if (dmabuf_texture_) {
        glDeleteTextures(1, &dmabuf_texture_);
        dmabuf_texture_ = 0;
    }
    use_dmabuf_ = false;
    dmabuf_width_ = 0;
    dmabuf_height_ = 0;
#endif

    // Clean up CEF texture
    if (cef_texture_) {
        glDeleteTextures(1, &cef_texture_);
        cef_texture_ = 0;
        cef_texture_width_ = 0;
        cef_texture_height_ = 0;
    }

    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }

    ctx_ = nullptr;
}
