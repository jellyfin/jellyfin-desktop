#ifndef WAYLANDVULKANCONTEXT_H
#define WAYLANDVULKANCONTEXT_H

#ifdef USE_WAYLAND_SUBSURFACE

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <vector>

class WaylandVulkanContext
{
public:
    WaylandVulkanContext();
    ~WaylandVulkanContext();

    bool initialize(wl_display *display, wl_surface *surface);
    bool createSwapchain(int width, int height);
    bool recreateSwapchain(int width, int height);
    void cleanup();

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    VkQueue queue() const { return m_queue; }
    uint32_t queueFamily() const { return m_queueFamily; }
    VkSurfaceKHR surface() const { return m_surface; }
    VkSwapchainKHR swapchain() const { return m_swapchain; }
    VkFormat swapchainFormat() const { return m_swapchainFormat; }
    bool isHdr() const { return m_isHdr; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    const std::vector<VkImage>& swapchainImages() const { return m_swapchainImages; }
    const std::vector<VkImageView>& swapchainViews() const { return m_swapchainViews; }

    VkPhysicalDeviceFeatures2* features() { return &m_features2; }
    const char* const* deviceExtensions() const { return m_deviceExtensions; }
    int deviceExtensionCount() const { return m_deviceExtensionCount; }

private:
    bool createInstance();
    bool selectPhysicalDevice();
    bool createDevice();
    bool createVulkanSurface(wl_display *display, wl_surface *wlSurface);
    void destroySwapchain();
    void setHdrMetadata();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR m_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    bool m_isHdr = false;
    int m_width = 0;
    int m_height = 0;

    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainViews;

    VkPhysicalDeviceVulkan11Features m_vk11Features{};
    VkPhysicalDeviceVulkan12Features m_vk12Features{};
    VkPhysicalDeviceFeatures2 m_features2{};

    static const char* m_deviceExtensions[];
    static const int m_deviceExtensionCount;
};

#endif // USE_WAYLAND_SUBSURFACE
#endif // WAYLANDVULKANCONTEXT_H
