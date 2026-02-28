#pragma once

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "wayland-protocols/color-management-v1-client.h"
#include "wayland-protocols/viewporter-client.h"
#include "video_surface.h"
#include <atomic>
#include <vector>

struct SDL_Window;

class WaylandSubsurface : public VideoSurface {
public:
    WaylandSubsurface();
    ~WaylandSubsurface() override;

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
    wl_display* display() const { return wl_display_; }
    wl_surface* surface() const { return mpv_surface_; }
    VkFormat swapchainFormat() const override { return swapchain_format_; }
    VkExtent2D swapchainExtent() const override { return swapchain_extent_; }
    bool isHdr() const override { return is_hdr_; }
    uint32_t width() const override { return swapchain_extent_.width; }
    uint32_t height() const override { return swapchain_extent_.height; }

    // Vulkan handles for mpv (our own device, not libplacebo)
    VkInstance vkInstance() const override { return instance_; }
    VkPhysicalDevice vkPhysicalDevice() const override { return physical_device_; }
    VkDevice vkDevice() const override { return device_; }
    VkQueue vkQueue() const override;
    uint32_t vkQueueFamily() const override;
    PFN_vkGetInstanceProcAddr vkGetProcAddr() const override { return vkGetInstanceProcAddr; }
    const VkPhysicalDeviceFeatures2* features() const override { return &features2_; }
    const char* const* deviceExtensions() const override;
    int deviceExtensionCount() const override;

    void commit();
    void hide() override;
    void setColorspace() override;
    void setDestinationSize(int width, int height) override;

    // Apply viewport destination immediately. Only call during init before
    // the render thread starts (no thread-safety guarantees).
    void initDestinationSize(int width, int height);

    // Wayland registry callbacks (public for C callback struct)
    static void registryGlobal(void* data, wl_registry* registry,
                               uint32_t name, const char* interface, uint32_t version);
    static void registryGlobalRemove(void* data, wl_registry* registry, uint32_t name);

private:
    bool initWayland(SDL_Window* window);
    bool createSubsurface(wl_surface* parentSurface);
    bool initColorManagement();
    void destroySwapchain();

    // Wayland
    wl_display* wl_display_ = nullptr;
    wl_compositor* wl_compositor_ = nullptr;
    wl_subcompositor* wl_subcompositor_ = nullptr;
    wl_surface* mpv_surface_ = nullptr;
    wl_subsurface* mpv_subsurface_ = nullptr;

    // Viewporter for HiDPI (render at physical, display at logical)
    wp_viewporter* viewporter_ = nullptr;
    wp_viewport* viewport_ = nullptr;

    // Color management
    wp_color_manager_v1* color_manager_ = nullptr;
    wp_color_management_surface_v1* color_surface_ = nullptr;
    wp_image_description_v1* hdr_image_desc_ = nullptr;

    // Vulkan (our own instance/device, like old jellyfin-desktop)
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
    VkFormat swapchain_format_ = VK_FORMAT_R16G16B16A16_UNORM;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapchain_extent_ = {0, 0};
    bool is_hdr_ = false;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    VkSemaphore image_available_ = VK_NULL_HANDLE;
    VkFence acquire_fence_ = VK_NULL_HANDLE;
    uint32_t current_image_idx_ = 0;
    bool frame_active_ = false;

    // Pending viewport destination (written by main thread, applied in recreateSwapchain)
    std::atomic<int> pending_dest_width_{0};
    std::atomic<int> pending_dest_height_{0};
    std::atomic<bool> dest_pending_{false};
};
