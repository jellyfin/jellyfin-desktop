#pragma once
#ifdef _WIN32

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include "video_surface.h"
#include <vector>
#include <mutex>

struct SDL_Window;

class WindowsVideoSurface : public VideoSurface {
public:
    WindowsVideoSurface();
    ~WindowsVideoSurface() override;

    bool init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physicalDevice,
              VkDevice device, uint32_t queueFamily,
              const char* const* extensions, int numExtensions,
              const VkPhysicalDeviceFeatures2* features) override;
    bool createSwapchain(int width, int height) override;
    bool recreateSwapchain(int width, int height) override;
    void cleanup() override;

    // Frame acquisition
    bool startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat) override;
    void submitFrame() override;

    // Accessors
    VkFormat swapchainFormat() const override { return VK_FORMAT_B8G8R8A8_UNORM; }
    VkExtent2D swapchainExtent() const override { return {width_, height_}; }
    bool isHdr() const override { return false; }
    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }

    // Vulkan handles for mpv
    VkInstance vkInstance() const override { return instance_; }
    VkPhysicalDevice vkPhysicalDevice() const override { return physical_device_; }
    VkDevice vkDevice() const override { return device_; }
    VkQueue vkQueue() const override { return queue_; }
    uint32_t vkQueueFamily() const override { return queue_family_; }
    PFN_vkGetInstanceProcAddr vkGetProcAddr() const override { return vkGetInstanceProcAddr; }
    const VkPhysicalDeviceFeatures2* features() const override { return &features2_; }
    const char* const* deviceExtensions() const override { return enabled_extensions_.data(); }
    int deviceExtensionCount() const override { return static_cast<int>(enabled_extensions_.size()); }

    // Non-swapchain image: use GENERAL layout instead of PRESENT_SRC_KHR
    VkImageLayout targetImageLayout() const override { return VK_IMAGE_LAYOUT_GENERAL; }

    void show() override;
    void hide() override;

private:
    bool initD3D11(SDL_Window* window);
    bool initVulkan();
    bool createSharedResources(int width, int height);
    void destroySharedResources();
    void destroySwapchain();

    SDL_Window* parent_window_ = nullptr;
    HWND parent_hwnd_ = nullptr;

    // D3D11
    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_context_ = nullptr;
    IDXGISwapChain1* swap_chain_ = nullptr;
    ID3D11Texture2D* staging_texture_ = nullptr;
    HANDLE shared_handle_ = nullptr;

    // DComp
    IDCompositionDevice* dcomp_device_ = nullptr;
    IDCompositionTarget* dcomp_target_ = nullptr;
    IDCompositionVisual* root_visual_ = nullptr;
    IDCompositionVisual* video_visual_ = nullptr;

    // D3D11 adapter LUID (for matching Vulkan physical device)
    LUID adapter_luid_ = {};

    // Vulkan
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;

    // Features/extensions for mpv
    VkPhysicalDeviceVulkan11Features vk11_features_{};
    VkPhysicalDeviceVulkan12Features vk12_features_{};
    VkPhysicalDeviceFeatures2 features2_{};
    std::vector<const char*> enabled_extensions_;

    // Shared Vulkan image (backed by imported D3D11 memory)
    VkImage shared_image_ = VK_NULL_HANDLE;
    VkImageView shared_view_ = VK_NULL_HANDLE;
    VkDeviceMemory imported_memory_ = VK_NULL_HANDLE;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool visible_ = false;
    bool frame_active_ = false;
};

#endif // _WIN32
