#pragma once
#ifdef __APPLE__

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

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

    bool createSwapchain(uint32_t width, uint32_t height);
    void destroySwapchain();

    VkSurfaceKHR surface() const { return surface_; }
    VkSwapchainKHR swapchain() const { return swapchain_; }
    VkFormat format() const { return format_; }
    VkColorSpaceKHR colorSpace() const { return color_space_; }
    uint32_t imageCount() const { return image_count_; }
    VkImage image(uint32_t index) const { return images_[index]; }

    // Frame acquisition (matches WaylandSubsurface interface)
    bool startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat);
    void submitFrame();

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

    void resize(uint32_t width, uint32_t height);
    void setVisible(bool visible);
    void hide() { setVisible(false); }
    void setPosition(int x, int y);

    bool isHdr() const { return is_hdr_; }
    void setColorspace() {}  // macOS EDR is automatic
    void setDestinationSize(int, int) {}  // no-op on macOS

    // For mpv render context
#ifdef __OBJC__
    void* getMetalLayer() { return (__bridge void*)metal_layer_; }
#else
    void* getMetalLayer() { return metal_layer_; }
#endif

private:
    SDL_Window* window_ = nullptr;
    NSView* video_view_ = nullptr;
    CAMetalLayer* metal_layer_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    static constexpr uint32_t MAX_IMAGES = 4;
    VkImage images_[MAX_IMAGES] = {};
    VkImageView image_views_[MAX_IMAGES] = {};
    uint32_t image_count_ = 0;
    uint32_t current_image_idx_ = 0;
    bool frame_active_ = false;

    VkSemaphore image_available_ = VK_NULL_HANDLE;
    VkSemaphore render_finished_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool is_hdr_ = false;
    bool needs_swapchain_recreate_ = false;

    // Features/extensions for mpv (must persist for the feature chain)
    VkPhysicalDeviceFeatures2 features2_{};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features_{};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_features_{};
    VkPhysicalDeviceHostQueryResetFeatures host_query_reset_features_{};
    const char* const* device_extensions_ = nullptr;
    int device_extension_count_ = 0;
};

#endif // __APPLE__
