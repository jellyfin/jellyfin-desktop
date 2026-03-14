#pragma once
#ifdef _WIN32

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <mutex>
#include <atomic>

// Renders a single CEF browser to a DComp visual via shared D3D11 textures.
// Each browser (main, overlay) gets its own instance with its own swap chain.
// Popups (dropdowns) get a separate child visual positioned via SetOffsetX/Y.
class DCompBrowserLayer {
public:
    DCompBrowserLayer();
    ~DCompBrowserLayer();

    // Initialize with shared DComp/D3D11 infrastructure from WindowsVideoSurface.
    // parent_visual is where this layer's visual will be added as a child.
    bool init(IDCompositionDevice* dcomp_device,
              IDCompositionVisual* parent_visual,
              ID3D11Device* d3d_device,
              ID3D11DeviceContext* d3d_context,
              std::mutex* d3d_mutex,
              int width, int height);

    // Called from CEF's OnAcceleratedPaint for PET_VIEW.
    // Opens the shared texture handle, copies to swap chain, presents.
    void onPaintView(HANDLE shared_texture_handle, int width, int height);

    // Called from CEF's OnAcceleratedPaint for PET_POPUP.
    // Opens the shared texture handle, copies to popup swap chain, presents.
    void onPaintPopup(HANDLE shared_texture_handle, int width, int height);

    // Called from CEF's OnPopupShow.
    void onPopupShow(bool show);

    // Called from CEF's OnPopupSize (CSS logical coordinates).
    void onPopupSize(int x, int y, int width, int height);

    // Set scale factor for popup positioning (physical / logical).
    void setScale(float scale) { scale_ = scale; }

    // Add/remove this layer's visual from its parent.
    void show();
    void hide();

    // Resize the browser swap chain.
    void resize(int width, int height);

    // Returns the browser visual (for adding child layers).
    IDCompositionVisual* visual() const { return browser_visual_; }

    void cleanup();

private:
    // Shared helpers
    bool createSwapChainFor(int width, int height, IDXGISwapChain1** out_chain, IDCompositionVisual* visual);
    void destroySwapChain(IDXGISwapChain1*& chain);
    bool copyAndPresent(HANDLE shared_texture_handle, IDXGISwapChain1* chain);

    // Shared references (not owned)
    IDCompositionDevice* dcomp_device_ = nullptr;
    IDCompositionVisual* parent_visual_ = nullptr;
    ID3D11Device* d3d_device_ = nullptr;
    ID3D11Device1* d3d_device1_ = nullptr;  // For OpenSharedResource1
    ID3D11DeviceContext* d3d_context_ = nullptr;
    std::mutex* d3d_mutex_ = nullptr;
    IDXGIFactory2* dxgi_factory_ = nullptr;  // Cached, obtained once in init()

    // Browser visual and swap chain
    IDCompositionVisual* browser_visual_ = nullptr;
    IDXGISwapChain1* swap_chain_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    // Popup visual and swap chain
    IDCompositionVisual* popup_visual_ = nullptr;
    IDXGISwapChain1* popup_swap_chain_ = nullptr;
    int popup_width_ = 0;
    int popup_height_ = 0;
    bool popup_visible_ = false;
    bool popup_in_tree_ = false;

    // Popup position (CSS logical coords from OnPopupSize)
    int popup_x_ = 0;
    int popup_y_ = 0;
    float scale_ = 1.0f;

    bool visible_ = false;
    bool first_paint_logged_ = false;
};

#endif // _WIN32
