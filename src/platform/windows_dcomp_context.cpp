#ifdef _WIN32

#include "platform/windows_dcomp_context.h"
#include "platform/windows_dxgi_util.h"
#include "logging.h"
#include <SDL3/SDL.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

bool WindowsDCompContext::init(SDL_Window* window, const LUID* adapter_luid) {
    // Get HWND from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    HWND hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd) {
        LOG_ERROR(LOG_PLATFORM, "[DCompContext] Failed to get HWND from SDL");
        return false;
    }

    // Find the DXGI adapter matching the Vulkan device's LUID
    IDXGIAdapter1* target_adapter = nullptr;
    if (adapter_luid) {
        target_adapter = findDxgiAdapterByLuid(*adapter_luid);
        if (!target_adapter)
            LOG_WARN(LOG_PLATFORM, "[DCompContext] No DXGI adapter matched Vulkan LUID, using default GPU");
    }

    // Create D3D11 device (on the matched adapter if found, default otherwise)
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
    D3D_FEATURE_LEVEL actualLevel;
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        target_adapter,
        target_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, createFlags, &featureLevel, 1,
        D3D11_SDK_VERSION, &d3d_device_, &actualLevel, &d3d_context_);
    if (FAILED(hr)) {
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            target_adapter,
            target_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, createFlags, &featureLevel, 1,
            D3D11_SDK_VERSION, &d3d_device_, &actualLevel, &d3d_context_);
    }
    if (target_adapter) target_adapter->Release();
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[DCompContext] D3D11CreateDevice failed: 0x%08lx", hr);
        return false;
    }

    // Enable multithread protection for D3D11 context sharing
    ID3D11Multithread* mt = nullptr;
    hr = d3d_device_->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt);
    if (SUCCEEDED(hr) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    // Create DComp device
    IDXGIDevice* dxgi_device = nullptr;
    hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[DCompContext] QueryInterface IDXGIDevice failed");
        return false;
    }

    hr = DCompositionCreateDevice(dxgi_device, __uuidof(IDCompositionDevice), (void**)&dcomp_device_);
    dxgi_device->Release();
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[DCompContext] DCompositionCreateDevice failed: 0x%08lx", hr);
        return false;
    }

    // Create DComp target (topmost=TRUE renders above child HWNDs)
    hr = dcomp_device_->CreateTargetForHwnd(hwnd, TRUE, &dcomp_target_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[DCompContext] CreateTargetForHwnd failed: 0x%08lx", hr);
        return false;
    }

    hr = dcomp_device_->CreateVisual(&root_visual_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[DCompContext] CreateVisual failed");
        return false;
    }
    dcomp_target_->SetRoot(root_visual_);

    // Enable per-pixel transparency via DWM
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    dcomp_device_->Commit();

    LOG_INFO(LOG_PLATFORM, "[DCompContext] D3D11 + DComp initialized (topmost overlay)");
    return true;
}

void WindowsDCompContext::cleanup() {
    if (root_visual_) { root_visual_->Release(); root_visual_ = nullptr; }
    if (dcomp_target_) { dcomp_target_->Release(); dcomp_target_ = nullptr; }
    if (dcomp_device_) { dcomp_device_->Release(); dcomp_device_ = nullptr; }
    if (d3d_context_) { d3d_context_->Release(); d3d_context_ = nullptr; }
    if (d3d_device_) { d3d_device_->Release(); d3d_device_ = nullptr; }
}

#endif // _WIN32
