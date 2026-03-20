#pragma once

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "wayland-protocols/color-management-v1-client.h"
#include "wayland-protocols/viewporter-client.h"
#include "video_surface.h"
#include <mpv/render_vk.h>
#include <atomic>
#include <vector>

struct SDL_Window;

// Wayland subsurface with libplacebo-managed swapchain.
// We create VkInstance/VkDevice/VkSurface and pass the VkSurface to mpv.
// mpv's internal libplacebo creates the swapchain (identical to standalone mpv),
// handling format selection, color management, and display profile negotiation.
// We do NOT create our own swapchain or color management surface.
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

    // Not used — mpv handles frame acquisition/presentation via pl_swapchain
    bool startFrame(VkImage*, VkImageView*, VkFormat*) override { return false; }
    void submitFrame() override {}

    // Accessors
    wl_display* display() const { return wl_display_; }
    wl_surface* surface() const { return mpv_surface_; }
    VkFormat swapchainFormat() const override { return VK_FORMAT_UNDEFINED; }
    VkExtent2D swapchainExtent() const override { return swapchain_extent_; }
    bool isHdr() const override { return true; }
    uint32_t width() const override { return swapchain_extent_.width; }
    uint32_t height() const override { return swapchain_extent_.height; }

    // Vulkan handles for mpv
    VkInstance vkInstance() const override { return instance_; }
    VkPhysicalDevice vkPhysicalDevice() const override { return physical_device_; }
    VkDevice vkDevice() const override { return device_; }
    VkQueue vkQueue() const override;
    uint32_t vkQueueFamily() const override;
    PFN_vkGetInstanceProcAddr vkGetProcAddr() const override { return vkGetInstanceProcAddr; }
    const VkPhysicalDeviceFeatures2* features() const override { return &features2_; }
    const char* const* deviceExtensions() const override;
    int deviceExtensionCount() const override;

    VkSurfaceKHR vkSurface() const { return vk_surface_; }
    const mpv_display_profile& displayProfile() const { return display_profile_; }

    void commit();
    void hide() override;
    void setColorspace() override {}  // Mesa handles via swapchain
    void setDestinationSize(int width, int height) override;
    void initDestinationSize(int width, int height);

    static void registryGlobal(void* data, wl_registry* registry,
                               uint32_t name, const char* interface, uint32_t version);
    static void registryGlobalRemove(void* data, wl_registry* registry, uint32_t name);

private:
    bool initWayland(SDL_Window* window);
    bool createSubsurface(wl_surface* parentSurface);
    void queryDisplayProfile();

    wl_display* wl_display_ = nullptr;
    wl_compositor* wl_compositor_ = nullptr;
    wl_subcompositor* wl_subcompositor_ = nullptr;
    wl_surface* mpv_surface_ = nullptr;
    wl_subsurface* mpv_subsurface_ = nullptr;

    wp_viewporter* viewporter_ = nullptr;
    wp_viewport* viewport_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkSurfaceKHR vk_surface_ = VK_NULL_HANDLE;

    VkPhysicalDeviceVulkan11Features vk11_features_{};
    VkPhysicalDeviceVulkan12Features vk12_features_{};
    VkPhysicalDeviceFeatures2 features2_{};
    std::vector<const char*> enabled_extensions_;

    wp_color_manager_v1* color_manager_ = nullptr;
    wl_output* wl_output_ = nullptr;
    mpv_display_profile display_profile_ = {};

    VkExtent2D swapchain_extent_ = {0, 0};

    std::atomic<int> pending_dest_width_{0};
    std::atomic<int> pending_dest_height_{0};
    std::atomic<bool> dest_pending_{false};
};
