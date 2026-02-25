#pragma once
#ifdef _WIN32

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <mutex>

// GL types needed for interop handles
#include <GL/gl.h>

struct SDL_Window;
class WGLContext;

// WGL_NV_DX_interop2 function types
typedef HANDLE (WINAPI *PFNWGLDXOPENDEVICENVPROC)(void* dxDevice);
typedef BOOL   (WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE (WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void* dxObject, GLuint name, GLenum type, GLenum access);
typedef BOOL   (WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL   (WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE* hObjects);
typedef BOOL   (WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE* hObjects);

#ifndef WGL_ACCESS_WRITE_DISCARD_NV
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002
#endif

class WindowsVideoLayer {
public:
    WindowsVideoLayer();
    ~WindowsVideoLayer();

    bool init(SDL_Window* window);
    bool createSwapchain(int width, int height);
    void cleanup();

    // GL-DXGI interop
    bool initInterop(WGLContext* wgl);    // WGL-level: open device, register texture (any context)
    bool createFBO();                      // GL-level: create FBO (must be called on the context that will use it)
    void destroyFBO();                     // GL-level: delete FBO (must be called on the owning context)
    bool lockInterop();                    // wglDXLockObjectsNV + staging_mutex_
    void unlockInterop();                  // wglDXUnlockObjectsNV + staging_mutex_
    GLuint fbo() const { return fbo_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // DComp presentation (main thread)
    void present();          // CopyResource + swap chain Present
    void commit();           // IDCompositionDevice::Commit
    void show();             // Attach video visual
    void hide();             // Detach video visual
    void recreateSwapchain(int width, int height);

    // Cleanup interop registration (call with GL context current for GL objects)
    void destroyInterop();

private:
    void destroySwapchain();

    SDL_Window* parent_window_ = nullptr;
    HWND parent_hwnd_ = nullptr;

    // D3D11
    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_context_ = nullptr;
    IDXGISwapChain1* swap_chain_ = nullptr;
    ID3D11Texture2D* staging_texture_ = nullptr;    // GL-DXGI shared

    // DComp
    IDCompositionDevice* dcomp_device_ = nullptr;
    IDCompositionTarget* dcomp_target_ = nullptr;
    IDCompositionVisual* root_visual_ = nullptr;
    IDCompositionVisual* video_visual_ = nullptr;

    // GL-DXGI interop (WGL_NV_DX_interop2)
    HANDLE dx_interop_device_ = nullptr;            // wglDXOpenDeviceNV
    HANDLE dx_interop_texture_ = nullptr;           // wglDXRegisterObjectNV
    GLuint gl_texture_ = 0;
    GLuint fbo_ = 0;
    GLuint depth_rb_ = 0;

    // WGL_NV_DX_interop2 function pointers
    PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV_ = nullptr;
    PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV_ = nullptr;
    PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV_ = nullptr;
    PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV_ = nullptr;
    PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV_ = nullptr;
    PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV_ = nullptr;

    // Synchronization
    std::mutex staging_mutex_;

    int width_ = 0;
    int height_ = 0;
    bool visible_ = false;
};

#endif // _WIN32
