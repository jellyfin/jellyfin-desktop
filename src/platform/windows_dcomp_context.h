#pragma once
#ifdef _WIN32

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <mutex>

struct SDL_Window;

// D3D11 + DirectComposition infrastructure for CEF overlay rendering.
// The DComp target is created with topmost=TRUE on the parent HWND,
// so overlay content renders above the child HWND's Vulkan swapchain.
class WindowsDCompContext {
public:
    bool init(SDL_Window* window, const LUID* adapter_luid);
    void cleanup();

    IDCompositionDevice* dcompDevice() const { return dcomp_device_; }
    IDCompositionVisual* rootVisual() const { return root_visual_; }
    ID3D11Device* d3dDevice() const { return d3d_device_; }
    ID3D11DeviceContext* d3dContext() const { return d3d_context_; }
    std::mutex& d3dMutex() { return d3d_mutex_; }

private:
    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_context_ = nullptr;

    IDCompositionDevice* dcomp_device_ = nullptr;
    IDCompositionTarget* dcomp_target_ = nullptr;
    IDCompositionVisual* root_visual_ = nullptr;

    std::mutex d3d_mutex_;
};

#endif // _WIN32
