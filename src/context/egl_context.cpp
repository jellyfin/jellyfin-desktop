#include "context/egl_context.h"
#include "logging.h"
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <drm_fourcc.h>

#ifdef SDL_PLATFORM_LINUX
#include <wayland-egl.h>
#include <X11/Xlib.h>
#endif

EGLContext_::EGLContext_() = default;

EGLContext_::~EGLContext_() {
    cleanup();
}

bool EGLContext_::init(SDL_Window* window) {
    // Detect Wayland vs X11
    const char* videoDriver = SDL_GetCurrentVideoDriver();
    is_wayland_ = videoDriver && strcmp(videoDriver, "wayland") == 0;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);

    if (is_wayland_) {
        // Wayland path
        struct wl_display* wl_display = static_cast<struct wl_display*>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
        struct wl_surface* wl_surface = static_cast<struct wl_surface*>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));

        if (!wl_display || !wl_surface) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to get Wayland display/surface from SDL");
            return false;
        }

        display_ = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display, nullptr);
        if (display_ == EGL_NO_DISPLAY) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to get EGL display (Wayland)");
            return false;
        }

        // Initialize EGL
        EGLint major, minor;
        if (!eglInitialize(display_, &major, &minor)) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to initialize EGL");
            return false;
        }
        LOG_INFO(LOG_GL, "[EGL] Initialized EGL %d.%d (Wayland)", major, minor);

        // Bind OpenGL ES API
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to bind OpenGL ES API");
            return false;
        }

        // Choose config
        EGLint config_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_NONE
        };

        EGLint num_configs;
        if (!eglChooseConfig(display_, config_attribs, &config_, 1, &num_configs) || num_configs == 0) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to choose config");
            return false;
        }

        // Create context
        EGLint context_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 0,
            EGL_NONE
        };

        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
        if (context_ == EGL_NO_CONTEXT) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to create context");
            return false;
        }

        // Get window size in pixels (for HiDPI support)
        SDL_GetWindowSizeInPixels(window, &width_, &height_);

        // Create wayland-egl window and EGL surface at pixel size
        egl_window_ = wl_egl_window_create(wl_surface, width_, height_);
        if (!egl_window_) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to create wayland-egl window");
            return false;
        }

        surface_ = eglCreateWindowSurface(display_, config_, (EGLNativeWindowType)egl_window_, nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to create window surface");
            wl_egl_window_destroy(egl_window_);
            egl_window_ = nullptr;
            return false;
        }
    } else {
        // X11 path
        Display* x11_display = static_cast<Display*>(
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        Window x11_window = static_cast<Window>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));

        if (!x11_display || !x11_window) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to get X11 display/window from SDL");
            return false;
        }

        display_ = eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, x11_display, nullptr);
        if (display_ == EGL_NO_DISPLAY) {
            // Fallback to default display
            display_ = eglGetDisplay((EGLNativeDisplayType)x11_display);
        }
        if (display_ == EGL_NO_DISPLAY) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to get EGL display (X11)");
            return false;
        }

        // Initialize EGL
        EGLint major, minor;
        if (!eglInitialize(display_, &major, &minor)) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to initialize EGL");
            return false;
        }
        LOG_INFO(LOG_GL, "[EGL] Initialized EGL %d.%d (X11)", major, minor);

        // Bind OpenGL ES API
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to bind OpenGL ES API");
            return false;
        }

        // Choose config
        EGLint config_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_NONE
        };

        EGLint num_configs;
        if (!eglChooseConfig(display_, config_attribs, &config_, 1, &num_configs) || num_configs == 0) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to choose config");
            return false;
        }

        // Create context
        EGLint context_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 0,
            EGL_NONE
        };

        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
        if (context_ == EGL_NO_CONTEXT) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to create context");
            return false;
        }

        // Get window size in pixels (for HiDPI support)
        SDL_GetWindowSizeInPixels(window, &width_, &height_);

        // Create EGL surface directly from X11 window
        surface_ = eglCreateWindowSurface(display_, config_, (EGLNativeWindowType)x11_window, nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            LOG_ERROR(LOG_GL, "[EGL] Failed to create window surface (X11)");
            return false;
        }
    }

    // Make context current
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        LOG_ERROR(LOG_GL, "[EGL] Failed to make context current");
        return false;
    }

    // Enable vsync
    eglSwapInterval(display_, 1);

    LOG_INFO(LOG_GL, "[EGL] Context created successfully");
    LOG_INFO(LOG_GL, "[EGL] GL_VERSION: %s", glGetString(GL_VERSION));
    LOG_INFO(LOG_GL, "[EGL] GL_RENDERER: %s", glGetString(GL_RENDERER));

    return true;
}

void EGLContext_::cleanup() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
    if (egl_window_) {
        wl_egl_window_destroy(egl_window_);
        egl_window_ = nullptr;
    }
}

void EGLContext_::swapBuffers() {
    eglSwapBuffers(display_, surface_);
}

bool EGLContext_::resize(int width, int height) {
    if (width == width_ && height == height_) {
        return true;
    }
    width_ = width;
    height_ = height;
    if (is_wayland_ && egl_window_) {
        wl_egl_window_resize(egl_window_, width, height, 0, 0);
    }
    // X11 window resize is handled by SDL/X11VideoLayer
    return true;
}

EGLContext EGLContext_::createSharedContext() const {
    if (display_ == EGL_NO_DISPLAY || context_ == EGL_NO_CONTEXT) {
        return EGL_NO_CONTEXT;
    }

    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };

    EGLContext shared = eglCreateContext(display_, config_, context_, context_attribs);
    if (shared == EGL_NO_CONTEXT) {
        LOG_ERROR(LOG_GL, "[EGL] Failed to create shared context");
        return EGL_NO_CONTEXT;
    }
    LOG_INFO(LOG_GL, "[EGL] Created shared context");
    return shared;
}

void EGLContext_::destroyContext(EGLContext ctx) const {
    if (display_ != EGL_NO_DISPLAY && ctx != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, ctx);
    }
}

bool EGLContext_::makeCurrent(EGLContext ctx) const {
    if (display_ == EGL_NO_DISPLAY) return false;
    if (ctx == EGL_NO_CONTEXT) {
        return eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
    }
    // For shared context, use surfaceless rendering (EGL_NO_SURFACE)
    // This requires EGL_KHR_surfaceless_context extension
    return eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE;
}

bool EGLContext_::makeCurrentMain() const {
    if (display_ == EGL_NO_DISPLAY) return false;
    return eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
}

// GBM function typedefs for dlsym
struct gbm_device;
struct gbm_bo;
using PFN_gbm_create_device = gbm_device* (*)(int fd);
using PFN_gbm_device_destroy = void (*)(gbm_device*);
using PFN_gbm_bo_create = gbm_bo* (*)(gbm_device*, uint32_t width, uint32_t height, uint32_t format, uint32_t flags);
using PFN_gbm_bo_destroy = void (*)(gbm_bo*);
using PFN_gbm_bo_get_fd = int (*)(gbm_bo*);
using PFN_gbm_bo_get_stride = uint32_t (*)(gbm_bo*);

bool EGLContext_::supportsDmaBufImport() const {
    if (display_ == EGL_NO_DISPLAY) return false;

    // dlopen libgbm — if not installed, skip probe (Mesa users don't need it)
    void* gbm_lib = dlopen("libgbm.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!gbm_lib) {
        LOG_INFO(LOG_GL, "[EGL] libgbm not available, skipping dmabuf probe");
        return true;
    }

    auto fn_create_device = reinterpret_cast<PFN_gbm_create_device>(dlsym(gbm_lib, "gbm_create_device"));
    auto fn_device_destroy = reinterpret_cast<PFN_gbm_device_destroy>(dlsym(gbm_lib, "gbm_device_destroy"));
    auto fn_bo_create = reinterpret_cast<PFN_gbm_bo_create>(dlsym(gbm_lib, "gbm_bo_create"));
    auto fn_bo_destroy = reinterpret_cast<PFN_gbm_bo_destroy>(dlsym(gbm_lib, "gbm_bo_destroy"));
    auto fn_bo_get_fd = reinterpret_cast<PFN_gbm_bo_get_fd>(dlsym(gbm_lib, "gbm_bo_get_fd"));
    auto fn_bo_get_stride = reinterpret_cast<PFN_gbm_bo_get_stride>(dlsym(gbm_lib, "gbm_bo_get_stride"));
    if (!fn_create_device || !fn_device_destroy || !fn_bo_create ||
        !fn_bo_destroy || !fn_bo_get_fd || !fn_bo_get_stride) {
        LOG_WARN(LOG_GL, "[EGL] libgbm missing symbols, skipping dmabuf probe");
        dlclose(gbm_lib);
        return true;
    }

    auto fn_egl_create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    auto fn_egl_destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    auto fn_gl_image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!fn_egl_create_image || !fn_egl_destroy_image || !fn_gl_image_target) {
        LOG_WARN(LOG_GL, "[EGL] EGL image extensions not available");
        dlclose(gbm_lib);
        return false;
    }

    // Find the DRM render node for the GPU backing this EGL display
    int drm_fd = -1;
    auto fn_query_display = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDisplayAttribEXT"));
    auto fn_query_device_str = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
        eglGetProcAddress("eglQueryDeviceStringEXT"));
    if (fn_query_display && fn_query_device_str) {
        EGLAttrib device_attrib = 0;
        if (fn_query_display(display_, EGL_DEVICE_EXT, &device_attrib) && device_attrib) {
            auto egl_device = reinterpret_cast<EGLDeviceEXT>(device_attrib);
            const char* node = fn_query_device_str(egl_device, EGL_DRM_RENDER_NODE_FILE_EXT);
            if (node) {
                drm_fd = open(node, O_RDWR | O_CLOEXEC);
                if (drm_fd >= 0) {
                    LOG_INFO(LOG_GL, "[EGL] Dmabuf probe using render node: %s", node);
                }
            }
        }
    }
    // Fallback: scan render nodes if EGL device query isn't available
    if (drm_fd < 0) {
        for (int i = 128; i < 136; i++) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
            drm_fd = open(path, O_RDWR | O_CLOEXEC);
            if (drm_fd >= 0) break;
        }
    }
    if (drm_fd < 0) {
        LOG_WARN(LOG_GL, "[EGL] No DRM render node found, skipping dmabuf probe");
        dlclose(gbm_lib);
        return true;
    }

    bool result = false;
    gbm_device* device = fn_create_device(drm_fd);
    if (!device) {
        LOG_WARN(LOG_GL, "[EGL] gbm_create_device failed");
        close(drm_fd);
        dlclose(gbm_lib);
        return false;
    }

    // GBM_BO_USE_RENDERING = 0x0002
    gbm_bo* bo = fn_bo_create(device, 64, 64, DRM_FORMAT_ARGB8888, 0x0002);
    if (!bo) {
        LOG_WARN(LOG_GL, "[EGL] gbm_bo_create ARGB8888 failed");
        fn_device_destroy(device);
        close(drm_fd);
        dlclose(gbm_lib);
        return false;
    }

    int dmabuf_fd = fn_bo_get_fd(bo);
    uint32_t stride = fn_bo_get_stride(bo);
    if (dmabuf_fd < 0) {
        LOG_WARN(LOG_GL, "[EGL] gbm_bo_get_fd failed");
        fn_bo_destroy(bo);
        fn_device_destroy(device);
        close(drm_fd);
        dlclose(gbm_lib);
        return false;
    }

    EGLint attrs[] = {
        EGL_WIDTH, 64,
        EGL_HEIGHT, 64,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(stride),
        EGL_NONE
    };
    // eglCreateImageKHR takes ownership of dmabuf_fd on success
    EGLImageKHR image = fn_egl_create_image(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (!image) {
        LOG_WARN(LOG_GL, "[EGL] Dmabuf probe: eglCreateImageKHR failed (0x%x)", eglGetError());
        close(dmabuf_fd);
    } else {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        fn_gl_image_target(GL_TEXTURE_2D, image);
        GLenum err = glGetError();
        if (err == GL_NO_ERROR) {
            result = true;
        } else {
            LOG_WARN(LOG_GL, "[EGL] Dmabuf probe: glEGLImageTargetTexture2DOES failed (0x%x)", err);
        }
        glDeleteTextures(1, &tex);
        fn_egl_destroy_image(display_, image);
    }

    fn_bo_destroy(bo);
    fn_device_destroy(device);
    close(drm_fd);
    dlclose(gbm_lib);

    if (result) {
        LOG_INFO(LOG_GL, "[EGL] Dmabuf probe: GBM -> EGL -> GL import OK");
    }
    return result;
}
