#pragma once
#ifdef _WIN32

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <GL/gl.h>
#include <mutex>

class WGLContext;

// Renders the CEF overlay to a DComp visual via WGL_NV_DX_interop2.
// This places CEF content ABOVE the video visual in the DComp tree,
// using premultiplied alpha so transparent areas show video through.
class WindowsOverlayLayer {
public:
    WindowsOverlayLayer();
    ~WindowsOverlayLayer();

    // Initialize with shared DComp/D3D11 infrastructure from the video surface.
    // parent_visual is the video visual — overlay is added as its child,
    // guaranteeing it renders ON TOP of video content (DComp parent-child rule).
    bool init(IDCompositionDevice* dcomp_device,
              IDCompositionVisual* parent_visual,
              ID3D11Device* d3d_device,
              ID3D11DeviceContext* d3d_context,
              std::mutex* d3d_mutex,
              WGLContext* wgl,
              int width, int height);

    // Begin frame: lock GL-D3D11 interop, bind FBO, clear to transparent.
    // Caller should then render CEF content (browsers.renderAll).
    void begin(int width, int height);

    // End frame: finish GL, unlock interop, copy to DComp swap chain, present.
    void end();

    void show();
    void hide();
    void cleanup();

private:
    bool createResources(int width, int height);
    void destroyResources();

    // Shared references (not owned)
    IDCompositionDevice* dcomp_device_ = nullptr;
    IDCompositionVisual* parent_visual_ = nullptr;  // video visual — overlay is its child
    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_context_ = nullptr;
    WGLContext* wgl_ = nullptr;
    std::mutex* d3d_mutex_ = nullptr;

    // Owned resources
    IDCompositionVisual* overlay_visual_ = nullptr;
    IDXGISwapChain1* swap_chain_ = nullptr;
    ID3D11Texture2D* staging_texture_ = nullptr;

    // GL interop
    GLuint gl_texture_ = 0;
    GLuint fbo_ = 0;
    HANDLE dx_interop_device_ = nullptr;
    HANDLE dx_interop_texture_ = nullptr;

    // WGL_NV_DX_interop2 function pointers
    using PFNWGLDXOPENDEVICENVPROC = HANDLE(WINAPI*)(void*);
    using PFNWGLDXCLOSEDEVICENVPROC = BOOL(WINAPI*)(HANDLE);
    using PFNWGLDXREGISTEROBJECTNVPROC = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
    using PFNWGLDXUNREGISTEROBJECTNVPROC = BOOL(WINAPI*)(HANDLE, HANDLE);
    using PFNWGLDXLOCKOBJECTSNVPROC = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
    using PFNWGLDXUNLOCKOBJECTSNVPROC = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);

    PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV_ = nullptr;
    PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV_ = nullptr;
    PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV_ = nullptr;
    PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV_ = nullptr;
    PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV_ = nullptr;
    PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    bool visible_ = false;
};

#endif // _WIN32
