#pragma once
#ifdef __APPLE__

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <mpv/render_vk.h>

// Forward declarations
#ifdef __OBJC__
@class CAMetalLayer;
@class NSView;
#else
typedef void CAMetalLayer;
typedef void NSView;
#endif

class MacOSVideoLayer {
public:
    bool init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physicalDevice,
              VkDevice device, uint32_t queueFamily,
              const char* const* deviceExtensions, uint32_t deviceExtensionCount,
              const char* const* instanceExtensions);
    void cleanup();

    // No-op: libplacebo manages the swapchain. Just stores dimensions.
    bool createSwapchain(uint32_t width, uint32_t height);

    // Accessors
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    VkDevice vkDevice() const { return device_; }
    VkInstance vkInstance() const { return instance_; }
    VkPhysicalDevice vkPhysicalDevice() const { return physical_device_; }
    VkQueue vkQueue() const { return queue_; }
    uint32_t vkQueueFamily() const { return queue_family_; }
    PFN_vkGetInstanceProcAddr vkGetProcAddr() const { return vkGetInstanceProcAddr; }
    const VkPhysicalDeviceFeatures2* features() const { return &features2_; }
    const char* const* deviceExtensions() const { return device_extensions_; }
    int deviceExtensionCount() const { return device_extension_count_; }

    VkSurfaceKHR vkSurface() const { return surface_; }
    const mpv_display_profile& displayProfile() const { return display_profile_; }

    void resize(uint32_t width, uint32_t height);
    void setVisible(bool visible);
    void show() { setVisible(true); }
    void hide() { setVisible(false); }
    void setPosition(int x, int y);

    bool isHdr() const { return is_hdr_; }
    void setColorspace() {}  // macOS EDR is automatic
    void setDestinationSize(int, int) {}
    void setSwapchain(const void*) {}  // macOS doesn't re-hint

private:
    void queryDisplayProfile();

    SDL_Window* window_ = nullptr;
    NSView* video_view_ = nullptr;
    CAMetalLayer* metal_layer_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool is_hdr_ = false;

    mpv_display_profile display_profile_ = {};

    // Features/extensions for mpv (must persist for the feature chain)
    VkPhysicalDeviceFeatures2 features2_{};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features_{};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_features_{};
    VkPhysicalDeviceHostQueryResetFeatures host_query_reset_features_{};
    const char* const* device_extensions_ = nullptr;
    int device_extension_count_ = 0;
};

#endif // __APPLE__
