#ifdef _WIN32

#include "context/wgl_context.h"
#include "context/gl_loader.h"
#include "logging.h"

// WGL_ARB_pixel_format constants
#define WGL_DRAW_TO_WINDOW_ARB         0x2001
#define WGL_SUPPORT_OPENGL_ARB         0x2010
#define WGL_DOUBLE_BUFFER_ARB          0x2011
#define WGL_PIXEL_TYPE_ARB             0x2013
#define WGL_COLOR_BITS_ARB             0x2014
#define WGL_ALPHA_BITS_ARB             0x201B
#define WGL_DEPTH_BITS_ARB             0x2022
#define WGL_TYPE_RGBA_ARB              0x202B

typedef BOOL (WINAPI *PFNWGLCHOOSEPIXELFORMATARBPROC)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);

WGLContext::WGLContext() = default;

WGLContext::~WGLContext() {
    cleanup();
}

// Bootstrap a temporary WGL context to load wglChoosePixelFormatARB,
// then choose a pixel format with an explicit 8-bit alpha channel.
// Legacy ChoosePixelFormat often returns formats with 0 alpha bits
// (it treats cAlphaBits as a hint), which breaks DWM per-pixel transparency.
static int choosePixelFormatARB(HDC targetDC) {
    // Register a temporary window class
    HINSTANCE hInst = GetModuleHandle(nullptr);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = hInst;
    wc.lpszClassName = "WGLBootstrap";
    RegisterClassA(&wc);

    HWND tmpHwnd = CreateWindowA("WGLBootstrap", "", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);
    HDC tmpDC = GetDC(tmpHwnd);

    // Set a basic pixel format on the temp window (required before wglCreateContext)
    PIXELFORMATDESCRIPTOR tmpPfd = {};
    tmpPfd.nSize = sizeof(tmpPfd);
    tmpPfd.nVersion = 1;
    tmpPfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    tmpPfd.iPixelType = PFD_TYPE_RGBA;
    tmpPfd.cColorBits = 32;
    int tmpFormat = ChoosePixelFormat(tmpDC, &tmpPfd);
    SetPixelFormat(tmpDC, tmpFormat, &tmpPfd);

    HGLRC tmpCtx = wglCreateContext(tmpDC);
    wglMakeCurrent(tmpDC, tmpCtx);

    // Load the ARB pixel format chooser
    auto wglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)
        wglGetProcAddress("wglChoosePixelFormatARB");

    int result = 0;
    if (wglChoosePixelFormatARB) {
        const int attribs[] = {
            WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
            WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
            WGL_DOUBLE_BUFFER_ARB,  GL_TRUE,
            WGL_PIXEL_TYPE_ARB,     WGL_TYPE_RGBA_ARB,
            WGL_COLOR_BITS_ARB,     32,
            WGL_ALPHA_BITS_ARB,     8,
            WGL_DEPTH_BITS_ARB,     0,
            0
        };
        UINT numFormats = 0;
        if (wglChoosePixelFormatARB(targetDC, attribs, nullptr, 1, &result, &numFormats)
            && numFormats > 0) {
            LOG_INFO(LOG_GL, "[WGL] wglChoosePixelFormatARB selected format %d", result);
        } else {
            LOG_WARN(LOG_GL, "[WGL] wglChoosePixelFormatARB failed, will use legacy fallback");
            result = 0;
        }
    } else {
        LOG_WARN(LOG_GL, "[WGL] wglChoosePixelFormatARB not available");
    }

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(tmpCtx);
    ReleaseDC(tmpHwnd, tmpDC);
    DestroyWindow(tmpHwnd);
    UnregisterClassA("WGLBootstrap", hInst);

    return result;
}

bool WGLContext::init(SDL_Window* window) {
    window_ = window;

    // Get HWND from SDL3
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    hwnd_ = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd_) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to get HWND from SDL window");
        return false;
    }

    hdc_ = GetDC(hwnd_);
    if (!hdc_) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to get DC");
        return false;
    }

    // Try to get a pixel format with explicit 8-bit alpha (needed for DWM transparency)
    int pixelFormat = choosePixelFormatARB(hdc_);

    // Fallback to legacy ChoosePixelFormat
    if (!pixelFormat) {
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        pfd.cDepthBits = 0;
        pfd.iLayerType = PFD_MAIN_PLANE;
        pixelFormat = ChoosePixelFormat(hdc_, &pfd);
    }

    // SetPixelFormat requires a PFD but only uses it for GDI; the format
    // index from wglChoosePixelFormatARB is what actually matters.
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    if (!pixelFormat || !SetPixelFormat(hdc_, pixelFormat, &pfd)) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to set pixel format");
        return false;
    }

    // Log actual pixel format for diagnostics
    PIXELFORMATDESCRIPTOR actual = {};
    DescribePixelFormat(hdc_, pixelFormat, sizeof(actual), &actual);
    LOG_INFO(LOG_GL, "[WGL] Pixel format %d: color=%d red=%d green=%d blue=%d alpha=%d depth=%d",
             pixelFormat, actual.cColorBits, actual.cRedBits, actual.cGreenBits,
             actual.cBlueBits, actual.cAlphaBits, actual.cDepthBits);

    // Create OpenGL context
    hglrc_ = wglCreateContext(hdc_);
    if (!hglrc_) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to create WGL context");
        return false;
    }

    makeCurrent();

    // Load GL extension functions
    if (!gl::initGLLoader()) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to load GL extensions");
        return false;
    }

    // Get window size
    SDL_GetWindowSize(window, &width_, &height_);

    LOG_INFO(LOG_GL, "[WGL] Context created successfully");
    LOG_INFO(LOG_GL, "[WGL] GL_VERSION: %s", glGetString(GL_VERSION));
    LOG_INFO(LOG_GL, "[WGL] GL_RENDERER: %s", glGetString(GL_RENDERER));

    return true;
}

void WGLContext::cleanup() {
    if (hglrc_) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hglrc_);
        hglrc_ = nullptr;
    }
    if (hdc_ && hwnd_) {
        ReleaseDC(hwnd_, hdc_);
        hdc_ = nullptr;
    }
}

void WGLContext::makeCurrent() {
    if (hdc_ && hglrc_) {
        wglMakeCurrent(hdc_, hglrc_);
    }
}

void WGLContext::swapBuffers() {
    if (hdc_) {
        SwapBuffers(hdc_);
    }
}

bool WGLContext::resize(int width, int height) {
    if (width == width_ && height == height_) {
        return true;
    }
    width_ = width;
    height_ = height;
    // WGL doesn't need explicit resize handling - the DC is tied to the HWND
    return true;
}

void* WGLContext::getProcAddress(const char* name) {
    // Try wglGetProcAddress first (for extensions)
    void* proc = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (proc) {
        return proc;
    }
    // Fall back to GetProcAddress from opengl32.dll (for core GL functions)
    static HMODULE opengl32 = LoadLibraryA("opengl32.dll");
    if (opengl32) {
        return reinterpret_cast<void*>(GetProcAddress(opengl32, name));
    }
    return nullptr;
}

HGLRC WGLContext::createSharedContext() const {
    if (!hdc_ || !hglrc_) {
        return nullptr;
    }

    // Create a new context and share lists with the main context
    HGLRC shared = wglCreateContext(hdc_);
    if (!shared) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to create shared context");
        return nullptr;
    }

    // Share display lists (textures, VBOs, etc.) between contexts
    if (!wglShareLists(hglrc_, shared)) {
        LOG_ERROR(LOG_GL, "[WGL] Failed to share lists between contexts");
        wglDeleteContext(shared);
        return nullptr;
    }

    LOG_INFO(LOG_GL, "[WGL] Created shared context");
    return shared;
}

void WGLContext::destroyContext(HGLRC ctx) const {
    if (ctx) {
        wglDeleteContext(ctx);
    }
}

bool WGLContext::makeCurrent(HGLRC ctx) const {
    if (!hdc_) return false;
    if (!ctx) {
        return wglMakeCurrent(nullptr, nullptr) == TRUE;
    }
    return wglMakeCurrent(hdc_, ctx) == TRUE;
}

bool WGLContext::makeCurrentMain() const {
    if (!hdc_ || !hglrc_) return false;
    return wglMakeCurrent(hdc_, hglrc_) == TRUE;
}

#endif // _WIN32
