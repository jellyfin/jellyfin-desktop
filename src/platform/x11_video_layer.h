#pragma once

#if defined(__linux__) && !defined(__ANDROID__)

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <X11/Xlib.h>
#include "video_surface.h"
#include <vector>

struct SDL_Window;

class X11VideoLayer : public VideoSurface {
public:
    X11VideoLayer();
    ~X11VideoLayer() override;

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
    VkFormat swapchainFormat() const override { return swapchain_format_; }
    VkExtent2D swapchainExtent() const override { return swapchain_extent_; }
    bool isHdr() const override { return false; }  // X11 has no standard HDR support
    uint32_t width() const override { return swapchain_extent_.width; }
    uint32_t height() const override { return swapchain_extent_.height; }

    // Vulkan handles for mpv
    VkInstance vkInstance() const override { return instance_; }
    VkPhysicalDevice vkPhysicalDevice() const override { return physical_device_; }
    VkDevice vkDevice() const override { return device_; }
    VkQueue vkQueue() const override { return queue_; }
    uint32_t vkQueueFamily() const override { return queue_family_; }
    PFN_vkGetInstanceProcAddr vkGetProcAddr() const override { return vkGetInstanceProcAddr; }
    const VkPhysicalDeviceFeatures2* features() const override { return &features2_; }
    const char* const* deviceExtensions() const override;
    int deviceExtensionCount() const override;

    void resize(int width, int height);

private:
    bool initX11(SDL_Window* window);
    void destroySwapchain();

    // X11
    Display* display_ = nullptr;
    Window parent_window_ = 0;
    Window video_window_ = 0;

    // Vulkan
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkSurfaceKHR vk_surface_ = VK_NULL_HANDLE;

    // Features/extensions for mpv
    VkPhysicalDeviceVulkan11Features vk11_features_{};
    VkPhysicalDeviceVulkan12Features vk12_features_{};
    VkPhysicalDeviceFeatures2 features2_{};
    std::vector<const char*> enabled_extensions_;

    // Swapchain
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchain_extent_ = {0, 0};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    VkSemaphore image_available_ = VK_NULL_HANDLE;
    VkFence acquire_fence_ = VK_NULL_HANDLE;
    uint32_t current_image_idx_ = 0;
    bool frame_active_ = false;
};

#endif // __linux__
