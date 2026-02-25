#ifdef _WIN32

#include "platform/windows_video_layer.h"
#include "context/wgl_context.h"
#include "context/gl_loader.h"
#include "logging.h"
#include <SDL3/SDL.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

WindowsVideoLayer::WindowsVideoLayer() = default;

WindowsVideoLayer::~WindowsVideoLayer() {
    cleanup();
}

bool WindowsVideoLayer::init(SDL_Window* window) {
    parent_window_ = window;

    // Get HWND from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    parent_hwnd_ = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!parent_hwnd_) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to get HWND from SDL");
        return false;
    }

    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL actualLevel;
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // No software rasterizer
        createFlags,
        &featureLevel, 1,
        D3D11_SDK_VERSION,
        &d3d_device_,
        &actualLevel,
        &d3d_context_
    );
    if (FAILED(hr)) {
        // Retry without debug layer
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               createFlags, &featureLevel, 1,
                               D3D11_SDK_VERSION, &d3d_device_, &actualLevel, &d3d_context_);
        if (FAILED(hr)) {
            LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] D3D11CreateDevice failed: 0x%08lx", hr);
            return false;
        }
    }
    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] D3D11 device created (feature level %d.%d)",
             (actualLevel >> 12) & 0xF, (actualLevel >> 8) & 0xF);

    // Get IDXGIDevice for DComp
    IDXGIDevice* dxgi_device = nullptr;
    hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] QueryInterface IDXGIDevice failed");
        return false;
    }

    // Create DirectComposition device
    hr = DCompositionCreateDevice(dxgi_device, __uuidof(IDCompositionDevice), (void**)&dcomp_device_);
    dxgi_device->Release();
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] DCompositionCreateDevice failed: 0x%08lx", hr);
        return false;
    }

    // Create composition target (topmost=FALSE -> behind window content)
    hr = dcomp_device_->CreateTargetForHwnd(parent_hwnd_, FALSE, &dcomp_target_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] CreateTargetForHwnd failed: 0x%08lx", hr);
        return false;
    }

    // Create root visual
    hr = dcomp_device_->CreateVisual(&root_visual_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] CreateVisual (root) failed");
        return false;
    }
    dcomp_target_->SetRoot(root_visual_);

    // Create video visual (child of root)
    hr = dcomp_device_->CreateVisual(&video_visual_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] CreateVisual (video) failed");
        return false;
    }
    // Don't add to root yet - show() will do that

    // Enable per-pixel transparency via DWM
    MARGINS margins = {-1, -1, -1, -1};
    hr = DwmExtendFrameIntoClientArea(parent_hwnd_, &margins);
    if (FAILED(hr)) {
        LOG_WARN(LOG_PLATFORM, "[WindowsVideoLayer] DwmExtendFrameIntoClientArea failed: 0x%08lx (transparency may not work)", hr);
    }

    dcomp_device_->Commit();

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] DComp initialized");
    return true;
}

bool WindowsVideoLayer::createSwapchain(int width, int height) {
    destroySwapchain();

    width_ = width;
    height_ = height;

    // Get DXGI factory from device
    IDXGIDevice* dxgi_device = nullptr;
    d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    IDXGIAdapter* adapter = nullptr;
    dxgi_device->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    adapter->Release();
    dxgi_device->Release();

    // Create composition swap chain (not bound to any HWND)
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;  // Opaque video

    HRESULT hr = factory->CreateSwapChainForComposition(d3d_device_, &desc, nullptr, &swap_chain_);
    factory->Release();
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] CreateSwapChainForComposition failed: 0x%08lx", hr);
        return false;
    }

    // Set swap chain as content of video visual
    video_visual_->SetContent(swap_chain_);

    // Create staging texture (shared with GL via interop)
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

    hr = d3d_device_->CreateTexture2D(&tex_desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] CreateTexture2D (staging) failed: 0x%08lx", hr);
        return false;
    }

    dcomp_device_->Commit();

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Swap chain created: %dx%d", width, height);
    return true;
}

bool WindowsVideoLayer::initInterop(WGLContext* wgl) {
    // Load WGL_NV_DX_interop2 functions (once)
    if (!wglDXOpenDeviceNV_) {
        wglDXOpenDeviceNV_ = (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
        wglDXCloseDeviceNV_ = (PFNWGLDXCLOSEDEVICENVPROC)wglGetProcAddress("wglDXCloseDeviceNV");
        wglDXRegisterObjectNV_ = (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXRegisterObjectNV");
        wglDXUnregisterObjectNV_ = (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXUnregisterObjectNV");
        wglDXLockObjectsNV_ = (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
        wglDXUnlockObjectsNV_ = (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");

        if (!wglDXOpenDeviceNV_ || !wglDXCloseDeviceNV_ ||
            !wglDXRegisterObjectNV_ || !wglDXUnregisterObjectNV_ ||
            !wglDXLockObjectsNV_ || !wglDXUnlockObjectsNV_) {
            LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] WGL_NV_DX_interop2 not available");
            return false;
        }
    }

    // Open D3D11 device for GL interop (once - persists across swap chain recreations)
    if (!dx_interop_device_) {
        dx_interop_device_ = wglDXOpenDeviceNV_(d3d_device_);
        if (!dx_interop_device_) {
            LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] wglDXOpenDeviceNV failed");
            return false;
        }
    }

    // Create GL texture and register with D3D11 staging texture
    glGenTextures(1, &gl_texture_);

    dx_interop_texture_ = wglDXRegisterObjectNV_(
        dx_interop_device_,
        staging_texture_,
        gl_texture_,
        GL_TEXTURE_2D,
        WGL_ACCESS_WRITE_DISCARD_NV
    );
    if (!dx_interop_texture_) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] wglDXRegisterObjectNV failed");
        return false;
    }

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] GL-DXGI interop initialized");
    return true;
}

bool WindowsVideoLayer::createFBO() {
    // Must be called on the GL context that will use the FBO (render thread).
    // The gl_texture_ is shared across contexts via wglShareLists.
    glGenFramebuffers(1, &fbo_);
    glGenRenderbuffers(1, &depth_rb_);

    // Must lock interop to attach the shared texture
    wglDXLockObjectsNV_(dx_interop_device_, 1, &dx_interop_texture_);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_texture_, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width_, height_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb_);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] FBO incomplete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        wglDXUnlockObjectsNV_(dx_interop_device_, 1, &dx_interop_texture_);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    wglDXUnlockObjectsNV_(dx_interop_device_, 1, &dx_interop_texture_);

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] FBO created on render thread");
    return true;
}

void WindowsVideoLayer::destroyFBO() {
    // Must be called on the GL context that owns the FBO
    if (depth_rb_) {
        glDeleteRenderbuffers(1, &depth_rb_);
        depth_rb_ = 0;
    }
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
}

bool WindowsVideoLayer::lockInterop() {
    staging_mutex_.lock();
    if (!wglDXLockObjectsNV_(dx_interop_device_, 1, &dx_interop_texture_)) {
        staging_mutex_.unlock();
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] wglDXLockObjectsNV failed");
        return false;
    }
    return true;
}

void WindowsVideoLayer::unlockInterop() {
    wglDXUnlockObjectsNV_(dx_interop_device_, 1, &dx_interop_texture_);
    staging_mutex_.unlock();
}

void WindowsVideoLayer::present() {
    std::lock_guard<std::mutex> lock(staging_mutex_);

    if (!swap_chain_ || !staging_texture_) return;

    // Get swap chain back buffer
    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back_buffer);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] GetBuffer failed: 0x%08lx", hr);
        return;
    }

    // Copy staging texture to back buffer
    d3d_context_->CopyResource(back_buffer, staging_texture_);
    back_buffer->Release();

    // Present the swap chain
    swap_chain_->Present(0, 0);
}

void WindowsVideoLayer::commit() {
    if (dcomp_device_) {
        dcomp_device_->Commit();
    }
}

void WindowsVideoLayer::show() {
    if (!visible_ && root_visual_ && video_visual_) {
        root_visual_->AddVisual(video_visual_, FALSE, nullptr);
        dcomp_device_->Commit();
        visible_ = true;
        LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Video visual shown");
    }
}

void WindowsVideoLayer::hide() {
    if (visible_ && root_visual_ && video_visual_) {
        root_visual_->RemoveVisual(video_visual_);
        dcomp_device_->Commit();
        visible_ = false;
        LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Video visual hidden");
    }
}

void WindowsVideoLayer::recreateSwapchain(int width, int height) {
    if (width == width_ && height == height_) return;

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Recreating swap chain: %dx%d -> %dx%d",
             width_, height_, width, height);

    // Hold staging_mutex_ for the entire operation to prevent present()
    // from accessing resources while they're being destroyed/recreated.
    // FBO must be destroyed separately by the caller via destroyFBO().
    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        destroyInterop();
        createSwapchain(width, height);
    }
}

void WindowsVideoLayer::destroySwapchain() {
    if (staging_texture_) {
        staging_texture_->Release();
        staging_texture_ = nullptr;
    }
    if (swap_chain_) {
        swap_chain_->Release();
        swap_chain_ = nullptr;
    }
}

void WindowsVideoLayer::destroyInterop() {
    // Unregister the D3D-GL texture mapping and delete the GL texture.
    // FBO must be destroyed separately via destroyFBO() on its owning context.
    if (dx_interop_texture_ && dx_interop_device_) {
        wglDXUnregisterObjectNV_(dx_interop_device_, dx_interop_texture_);
        dx_interop_texture_ = nullptr;
    }
    if (gl_texture_) {
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
    }
}

void WindowsVideoLayer::cleanup() {
    destroyFBO();
    destroyInterop();

    if (dx_interop_device_) {
        wglDXCloseDeviceNV_(dx_interop_device_);
        dx_interop_device_ = nullptr;
    }

    destroySwapchain();

    if (video_visual_) {
        video_visual_->Release();
        video_visual_ = nullptr;
    }
    if (root_visual_) {
        root_visual_->Release();
        root_visual_ = nullptr;
    }
    if (dcomp_target_) {
        dcomp_target_->Release();
        dcomp_target_ = nullptr;
    }
    if (dcomp_device_) {
        dcomp_device_->Release();
        dcomp_device_ = nullptr;
    }
    if (d3d_context_) {
        d3d_context_->Release();
        d3d_context_ = nullptr;
    }
    if (d3d_device_) {
        d3d_device_->Release();
        d3d_device_ = nullptr;
    }

    visible_ = false;
}

#endif // _WIN32
