#ifdef _WIN32

#include "platform/dcomp_browser_layer.h"
#include "logging.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

DCompBrowserLayer::DCompBrowserLayer() = default;

DCompBrowserLayer::~DCompBrowserLayer() {
    cleanup();
}

bool DCompBrowserLayer::init(IDCompositionDevice* dcomp_device,
                              IDCompositionVisual* parent_visual,
                              ID3D11Device* d3d_device,
                              ID3D11DeviceContext* d3d_context,
                              std::mutex* d3d_mutex,
                              int width, int height) {
    dcomp_device_ = dcomp_device;
    parent_visual_ = parent_visual;
    d3d_device_ = d3d_device;
    d3d_context_ = d3d_context;
    d3d_mutex_ = d3d_mutex;

    // Get ID3D11Device1 for OpenSharedResource1 (NT handle support)
    HRESULT hr = d3d_device_->QueryInterface(__uuidof(ID3D11Device1), (void**)&d3d_device1_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[DCompBrowserLayer] QueryInterface ID3D11Device1 failed: 0x%08lx", hr);
        return false;
    }

    // Cache DXGI factory (stable for device lifetime)
    IDXGIDevice* dxgi_device = nullptr;
    hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    if (FAILED(hr) || !dxgi_device) {
        LOG_ERROR(LOG_PLATFORM, "[DCompBrowserLayer] QueryInterface IDXGIDevice failed: 0x%08lx", hr);
        return false;
    }
    IDXGIAdapter* adapter = nullptr;
    hr = dxgi_device->GetAdapter(&adapter);
    dxgi_device->Release();
    if (FAILED(hr) || !adapter) {
        LOG_ERROR(LOG_PLATFORM, "[DCompBrowserLayer] GetAdapter failed: 0x%08lx", hr);
        return false;
    }
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&dxgi_factory_);
    adapter->Release();
    if (FAILED(hr) || !dxgi_factory_) {
        LOG_ERROR(LOG_PLATFORM, "[DCompBrowserLayer] GetParent IDXGIFactory2 failed: 0x%08lx", hr);
        return false;
    }

    // Create browser and popup visuals
    hr = dcomp_device_->CreateVisual(&browser_visual_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[DCompBrowserLayer] CreateVisual (browser) failed: 0x%08lx", hr);
        return false;
    }
    hr = dcomp_device_->CreateVisual(&popup_visual_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[DCompBrowserLayer] CreateVisual (popup) failed: 0x%08lx", hr);
        return false;
    }

    if (!createSwapChainFor(width, height, &swap_chain_, browser_visual_)) {
        return false;
    }
    width_ = width;
    height_ = height;

    LOG_INFO(LOG_PLATFORM, "[DCompBrowserLayer] Initialized %dx%d", width, height);
    return true;
}

bool DCompBrowserLayer::createSwapChainFor(int width, int height,
                                             IDXGISwapChain1** out_chain,
                                             IDCompositionVisual* visual) {
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = dxgi_factory_->CreateSwapChainForComposition(d3d_device_, &desc, nullptr, out_chain);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[DCompBrowserLayer] CreateSwapChainForComposition failed: 0x%08lx", hr);
        return false;
    }

    // Clear to transparent black (avoids white flash before first CEF paint)
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED((*out_chain)->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(d3d_device_->CreateRenderTargetView(bb, nullptr, &rtv))) {
            float black[4] = {0, 0, 0, 0};
            d3d_context_->ClearRenderTargetView(rtv, black);
            rtv->Release();
        }
        bb->Release();
        (*out_chain)->Present(0, 0);
    }

    visual->SetContent(*out_chain);
    return true;
}

void DCompBrowserLayer::destroySwapChain(IDXGISwapChain1*& chain) {
    if (chain) {
        chain->Release();
        chain = nullptr;
    }
}

bool DCompBrowserLayer::copyAndPresent(HANDLE shared_texture_handle, IDXGISwapChain1* chain) {
    ID3D11Texture2D* src_texture = nullptr;
    HRESULT hr = d3d_device1_->OpenSharedResource1(
        shared_texture_handle, __uuidof(ID3D11Texture2D), (void**)&src_texture);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[DCompBrowserLayer] OpenSharedResource1 failed: 0x%08lx", hr);
        return false;
    }

    ID3D11Texture2D* back_buffer = nullptr;
    hr = chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back_buffer);
    if (SUCCEEDED(hr)) {
        d3d_context_->CopyResource(back_buffer, src_texture);
        back_buffer->Release();
        chain->Present(0, 0);
        dcomp_device_->Commit();
    }

    src_texture->Release();
    return SUCCEEDED(hr);
}

void DCompBrowserLayer::onPaintView(HANDLE shared_texture_handle, int width, int height) {
    if (!d3d_device1_) return;

    std::lock_guard<std::mutex> lock(*d3d_mutex_);

    if (!swap_chain_) return;

    // Resize swap chain if CEF's texture size changed
    if (width != width_ || height != height_) {
        LOG_INFO(LOG_PLATFORM, "[DCompBrowserLayer] View resize: %dx%d -> %dx%d",
                 width_, height_, width, height);
        destroySwapChain(swap_chain_);
        if (!createSwapChainFor(width, height, &swap_chain_, browser_visual_)) return;
        width_ = width;
        height_ = height;
    }

    copyAndPresent(shared_texture_handle, swap_chain_);

    if (!first_paint_logged_) {
        LOG_INFO(LOG_PLATFORM, "[DCompBrowserLayer] First view paint: %dx%d", width, height);
        first_paint_logged_ = true;
    }
}

void DCompBrowserLayer::onPaintPopup(HANDLE shared_texture_handle, int width, int height) {
    if (!d3d_device1_ || !popup_visible_) return;

    std::lock_guard<std::mutex> lock(*d3d_mutex_);

    // Recreate popup swap chain if size changed
    if (width != popup_width_ || height != popup_height_) {
        destroySwapChain(popup_swap_chain_);
        popup_visual_->SetContent(nullptr);
        if (!createSwapChainFor(width, height, &popup_swap_chain_, popup_visual_)) return;
        popup_width_ = width;
        popup_height_ = height;
    }

    if (!popup_swap_chain_) return;

    // Position popup at scaled coordinates
    popup_visual_->SetOffsetX(static_cast<float>(popup_x_) * scale_);
    popup_visual_->SetOffsetY(static_cast<float>(popup_y_) * scale_);

    // Add popup visual to tree on first paint
    if (!popup_in_tree_) {
        browser_visual_->AddVisual(popup_visual_, FALSE, nullptr);
        popup_in_tree_ = true;
    }

    copyAndPresent(shared_texture_handle, popup_swap_chain_);
}

void DCompBrowserLayer::onPopupShow(bool show) {
    popup_visible_ = show;
    if (!show) {
        std::lock_guard<std::mutex> lock(*d3d_mutex_);
        if (popup_in_tree_) {
            browser_visual_->RemoveVisual(popup_visual_);
            popup_in_tree_ = false;
        }
        popup_visual_->SetContent(nullptr);
        destroySwapChain(popup_swap_chain_);
        popup_width_ = 0;
        popup_height_ = 0;
        dcomp_device_->Commit();
    }
}

void DCompBrowserLayer::onPopupSize(int x, int y, int width, int height) {
    popup_x_ = x;
    popup_y_ = y;
    (void)width;
    (void)height;
}

void DCompBrowserLayer::setOpacity(float alpha) {
    if (!dcomp_device_ || !browser_visual_) return;
    std::lock_guard<std::mutex> lock(*d3d_mutex_);

    if (!effect_group_) {
        HRESULT hr = dcomp_device_->CreateEffectGroup(&effect_group_);
        if (FAILED(hr)) {
            LOG_ERROR(LOG_PLATFORM, "[DCompBrowserLayer] CreateEffectGroup failed: 0x%08lx", hr);
            return;
        }
        browser_visual_->SetEffect(effect_group_);
    }

    effect_group_->SetOpacity(alpha);
    dcomp_device_->Commit();
}

void DCompBrowserLayer::show() {
    if (!visible_ && parent_visual_ && browser_visual_) {
        std::lock_guard<std::mutex> lock(*d3d_mutex_);
        parent_visual_->AddVisual(browser_visual_, FALSE, nullptr);
        dcomp_device_->Commit();
        visible_ = true;
        LOG_INFO(LOG_PLATFORM, "[DCompBrowserLayer] Visual shown");
    }
}

void DCompBrowserLayer::hide() {
    if (visible_ && parent_visual_ && browser_visual_) {
        std::lock_guard<std::mutex> lock(*d3d_mutex_);
        parent_visual_->RemoveVisual(browser_visual_);
        dcomp_device_->Commit();
        visible_ = false;
        LOG_INFO(LOG_PLATFORM, "[DCompBrowserLayer] Visual hidden");
    }
}

void DCompBrowserLayer::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    std::lock_guard<std::mutex> lock(*d3d_mutex_);
    LOG_INFO(LOG_PLATFORM, "[DCompBrowserLayer] Resize: %dx%d -> %dx%d",
             width_, height_, width, height);
    destroySwapChain(swap_chain_);
    createSwapChainFor(width, height, &swap_chain_, browser_visual_);
    width_ = width;
    height_ = height;
    dcomp_device_->Commit();
}

void DCompBrowserLayer::cleanup() {
    if (popup_in_tree_ && browser_visual_ && popup_visual_) {
        browser_visual_->RemoveVisual(popup_visual_);
        popup_in_tree_ = false;
    }
    destroySwapChain(popup_swap_chain_);

    if (visible_ && parent_visual_ && browser_visual_) {
        parent_visual_->RemoveVisual(browser_visual_);
        visible_ = false;
    }
    destroySwapChain(swap_chain_);

    if (popup_visual_) {
        popup_visual_->Release();
        popup_visual_ = nullptr;
    }
    if (effect_group_) {
        effect_group_->Release();
        effect_group_ = nullptr;
    }
    if (browser_visual_) {
        browser_visual_->Release();
        browser_visual_ = nullptr;
    }
    if (dxgi_factory_) {
        dxgi_factory_->Release();
        dxgi_factory_ = nullptr;
    }
    if (d3d_device1_) {
        d3d_device1_->Release();
        d3d_device1_ = nullptr;
    }
}

#endif // _WIN32
