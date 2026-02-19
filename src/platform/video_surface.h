#pragma once

#include <vulkan/vulkan.h>

struct SDL_Window;

// Abstract base class for video rendering surfaces (Wayland, X11, macOS, Windows)
class VideoSurface {
public:
    virtual ~VideoSurface() = default;

    virtual bool init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physicalDevice,
                      VkDevice device, uint32_t queueFamily,
                      const char* const* extensions, int numExtensions,
                      const VkPhysicalDeviceFeatures2* features) = 0;
    virtual bool createSwapchain(int width, int height) = 0;
    virtual bool recreateSwapchain(int width, int height) = 0;
    virtual void cleanup() = 0;

    // Frame acquisition
    virtual bool startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat) = 0;
    virtual void submitFrame() = 0;

    // Accessors
    virtual VkFormat swapchainFormat() const = 0;
    virtual VkExtent2D swapchainExtent() const = 0;
    virtual bool isHdr() const = 0;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;

    // Vulkan handles for mpv
    virtual VkInstance vkInstance() const = 0;
    virtual VkPhysicalDevice vkPhysicalDevice() const = 0;
    virtual VkDevice vkDevice() const = 0;
    virtual VkQueue vkQueue() const = 0;
    virtual uint32_t vkQueueFamily() const = 0;
    virtual PFN_vkGetInstanceProcAddr vkGetProcAddr() const = 0;
    virtual const VkPhysicalDeviceFeatures2* features() const = 0;
    virtual const char* const* deviceExtensions() const = 0;
    virtual int deviceExtensionCount() const = 0;

    virtual void setColorspace() {}  // Platform-specific colorspace setup (default no-op)
    virtual void setDestinationSize(int, int) {}  // HiDPI logical size (default no-op)
};
