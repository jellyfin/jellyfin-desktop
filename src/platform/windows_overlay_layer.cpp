#ifdef _WIN32

#include "platform/windows_overlay_layer.h"
#include "context/wgl_context.h"
#include "context/gl_loader.h"
#include "logging.h"

#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002

WindowsOverlayLayer::WindowsOverlayLayer() = default;

WindowsOverlayLayer::~WindowsOverlayLayer() {
    cleanup();
}

bool WindowsOverlayLayer::init(IDCompositionDevice* dcomp_device,
                                IDCompositionVisual* parent_visual,
                                ID3D11Device* d3d_device,
                                ID3D11DeviceContext* d3d_context,
                                std::mutex* d3d_mutex,
                                WGLContext* wgl,
                                int width, int height) {
    dcomp_device_ = dcomp_device;
    parent_visual_ = parent_visual;
    d3d_device_ = d3d_device;
    d3d_context_ = d3d_context;
    d3d_mutex_ = d3d_mutex;
    wgl_ = wgl;

    // Load WGL_NV_DX_interop2 functions
    wglDXOpenDeviceNV_ = (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV_ = (PFNWGLDXCLOSEDEVICENVPROC)wglGetProcAddress("wglDXCloseDeviceNV");
    wglDXRegisterObjectNV_ = (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV_ = (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV_ = (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV_ = (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");

    if (!wglDXOpenDeviceNV_ || !wglDXCloseDeviceNV_ ||
        !wglDXRegisterObjectNV_ || !wglDXUnregisterObjectNV_ ||
        !wglDXLockObjectsNV_ || !wglDXUnlockObjectsNV_) {
        LOG_ERROR(LOG_PLATFORM, "[OverlayLayer] WGL_NV_DX_interop2 not available");
        return false;
    }

    // Open D3D11 device for GL interop
    dx_interop_device_ = wglDXOpenDeviceNV_(d3d_device_);
    if (!dx_interop_device_) {
        LOG_ERROR(LOG_PLATFORM, "[OverlayLayer] wglDXOpenDeviceNV failed");
        return false;
    }

    // Create DComp visual for the overlay
    HRESULT hr = dcomp_device_->CreateVisual(&overlay_visual_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[OverlayLayer] CreateVisual failed: 0x%08lx", hr);
        return false;
    }

    if (!createResources(width, height)) {
        return false;
    }

    LOG_INFO(LOG_PLATFORM, "[OverlayLayer] Initialized %dx%d", width, height);
    return true;
}

bool WindowsOverlayLayer::createResources(int width, int height) {
    width_ = width;
    height_ = height;

    // Create D3D11 staging texture for GL interop (no sharing flags needed,
    // WGL_NV_DX_interop2 handles the sharing internally)
    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    tex_desc.MiscFlags = 0;

    HRESULT hr = d3d_device_->CreateTexture2D(&tex_desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[OverlayLayer] CreateTexture2D failed: 0x%08lx", hr);
        return false;
    }

    // Create GL texture and register with D3D11 via interop
    glGenTextures(1, &gl_texture_);
    dx_interop_texture_ = wglDXRegisterObjectNV_(
        dx_interop_device_, staging_texture_, gl_texture_,
        GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!dx_interop_texture_) {
        LOG_ERROR(LOG_PLATFORM, "[OverlayLayer] wglDXRegisterObjectNV failed");
        return false;
    }

    // Create FBO backed by the interop texture
    // Must lock interop to attach the texture
    wglDXLockObjectsNV_(dx_interop_device_, 1, &dx_interop_texture_);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_texture_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR(LOG_PLATFORM, "[OverlayLayer] FBO incomplete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        wglDXUnlockObjectsNV_(dx_interop_device_, 1, &dx_interop_texture_);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    wglDXUnlockObjectsNV_(dx_interop_device_, 1, &dx_interop_texture_);

    // Create DComp swap chain with premultiplied alpha
    IDXGIDevice* dxgi_device = nullptr;
    d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    IDXGIAdapter* adapter = nullptr;
    dxgi_device->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    adapter->Release();
    dxgi_device->Release();

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    hr = factory->CreateSwapChainForComposition(d3d_device_, &desc, nullptr, &swap_chain_);
    factory->Release();
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[OverlayLayer] CreateSwapChainForComposition failed: 0x%08lx", hr);
        return false;
    }

    overlay_visual_->SetContent(swap_chain_);
    dcomp_device_->Commit();

    LOG_INFO(LOG_PLATFORM, "[OverlayLayer] Resources created: %dx%d", width, height);
    return true;
}

void WindowsOverlayLayer::begin(int width, int height) {
    // Resize if needed
    if (width != width_ || height != height_) {
        LOG_INFO(LOG_PLATFORM, "[OverlayLayer] Resizing: %dx%d -> %dx%d",
                 width_, height_, width, height);
        destroyResources();
        createResources(width, height);
    }

    // Lock GL-D3D11 interop
    wglDXLockObjectsNV_(dx_interop_device_, 1, &dx_interop_texture_);

    // Bind FBO and clear to fully transparent
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void WindowsOverlayLayer::end() {
    // Ensure GL rendering is complete before releasing interop
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Unlock interop — D3D11 can now access the staging texture
    wglDXUnlockObjectsNV_(dx_interop_device_, 1, &dx_interop_texture_);

    // Lock D3D11/DComp — video render thread's submitFrame() uses the same context
    std::lock_guard<std::mutex> lock(*d3d_mutex_);

    // Copy staging texture to swap chain back buffer
    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back_buffer);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[OverlayLayer] GetBuffer failed: 0x%08lx", hr);
        return;
    }

    d3d_context_->CopyResource(back_buffer, staging_texture_);
    back_buffer->Release();

    swap_chain_->Present(0, 0);
    dcomp_device_->Commit();
}

void WindowsOverlayLayer::show() {
    if (!visible_ && parent_visual_ && overlay_visual_) {
        // Add as child of video visual — DComp children always render ON TOP
        // of their parent's content, guaranteeing overlay above video
        parent_visual_->AddVisual(overlay_visual_, TRUE, nullptr);
        dcomp_device_->Commit();
        visible_ = true;
        LOG_INFO(LOG_PLATFORM, "[OverlayLayer] Overlay visual shown (child of video)");
    }
}

void WindowsOverlayLayer::hide() {
    if (visible_ && parent_visual_ && overlay_visual_) {
        parent_visual_->RemoveVisual(overlay_visual_);
        dcomp_device_->Commit();
        visible_ = false;
        LOG_INFO(LOG_PLATFORM, "[OverlayLayer] Overlay visual hidden");
    }
}

void WindowsOverlayLayer::destroyResources() {
    if (dx_interop_texture_ && dx_interop_device_) {
        wglDXUnregisterObjectNV_(dx_interop_device_, dx_interop_texture_);
        dx_interop_texture_ = nullptr;
    }
    if (gl_texture_) {
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
    }
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (staging_texture_) {
        staging_texture_->Release();
        staging_texture_ = nullptr;
    }
    if (swap_chain_) {
        swap_chain_->Release();
        swap_chain_ = nullptr;
    }
}

void WindowsOverlayLayer::cleanup() {
    destroyResources();

    if (dx_interop_device_) {
        wglDXCloseDeviceNV_(dx_interop_device_);
        dx_interop_device_ = nullptr;
    }
    if (overlay_visual_) {
        overlay_visual_->Release();
        overlay_visual_ = nullptr;
    }

    visible_ = false;
}

#endif // _WIN32
