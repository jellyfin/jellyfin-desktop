#pragma once
#ifdef _WIN32

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <dxgi1_6.h>
#include <mpv/render_vk.h>
#include <vector>

struct SDL_Window;

// Video rendering layer for Windows.
// Creates a child HWND and Vulkan surface — libplacebo manages the swapchain.
// The DComp target (topmost=TRUE) on the parent HWND renders CEF above
// the child HWND's Vulkan swapchain content.
class WindowsVideoLayer {
public:
    bool init(SDL_Window* window);
    void cleanup();

    // No-op: libplacebo manages the swapchain via the VkSurface.
    bool createSwapchain(int width, int height);

    // Accessors for mpv render context
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    VkDevice vkDevice() const { return device_; }
    VkInstance vkInstance() const { return instance_; }
    VkPhysicalDevice vkPhysicalDevice() const { return physical_device_; }
    VkQueue vkQueue() const { return queue_; }
    uint32_t vkQueueFamily() const { return queue_family_; }
    PFN_vkGetInstanceProcAddr vkGetProcAddr() const { return vkGetInstanceProcAddr; }
    const VkPhysicalDeviceFeatures2* features() const { return &features2_; }
    const char* const* deviceExtensions() const { return enabled_extensions_.data(); }
    int deviceExtensionCount() const { return static_cast<int>(enabled_extensions_.size()); }

    VkSurfaceKHR vkSurface() const { return surface_; }
    const mpv_display_profile& displayProfile() const { return display_profile_; }

    void resize(uint32_t width, uint32_t height);
    void setVisible(bool visible);
    void show() { setVisible(true); }
    void hide() { setVisible(false); }

    bool isHdr() const { return is_hdr_; }
    void setColorspace() {}
    void setDestinationSize(int, int) {}
    void setSwapchain(const void*) {}  // Windows doesn't re-hint

    // DXGI adapter LUID — WindowsDCompContext uses this to create its
    // D3D11 device on the same GPU.
    const LUID& adapterLuid() const { return adapter_luid_; }

private:
    bool initVulkan();
    void queryDisplayProfile();

    SDL_Window* parent_window_ = nullptr;
    HWND parent_hwnd_ = nullptr;
    HWND child_hwnd_ = nullptr;

    // Vulkan
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;

    // Features/extensions for mpv
    VkPhysicalDeviceVulkan11Features vk11_features_{};
    VkPhysicalDeviceVulkan12Features vk12_features_{};
    VkPhysicalDeviceFeatures2 features2_{};
    std::vector<const char*> enabled_extensions_;

    LUID adapter_luid_ = {};

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool is_hdr_ = false;

    mpv_display_profile display_profile_ = {};
};

#endif // _WIN32
