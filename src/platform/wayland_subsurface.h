#pragma once

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "wayland-protocols/color-management-v1-client.h"
#include "wayland-protocols/linux-dmabuf-v1-client.h"
#include "wayland-protocols/viewporter-client.h"
#include "video_surface.h"
#include <mpv/render_vk.h>
#include <libplacebo/colorspace.h>
#include <drm/drm_fourcc.h>
#include <atomic>
#include <vector>

struct SDL_Window;

// Dmabuf buffer for zero-copy presentation on Wayland.
// Each buffer holds a VkImage (with dmabuf-exportable memory), the exported
// fd, and the corresponding wl_buffer for wl_surface_attach.
struct DmabufBuffer {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    wl_buffer* buffer = nullptr;
    int dmabuf_fd = -1;
    uint32_t stride = 0;
    bool busy = false;  // owned by compositor (not yet released)
};

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

    // Dmabuf presentation — replaces swapchain for video rendering
    bool initDmabufPool(uint32_t width, uint32_t height);
    DmabufBuffer* acquireBuffer();
    void presentBuffer(DmabufBuffer* buf);
    void destroyDmabufPool();

    // HDR signaling — set PQ/BT.2020 image description on parent surface.
    void activateColorManagement();
    void setHdrImageDescription(uint32_t max_luma = 0, uint32_t ref_luma = 0);

    // Called after render — reads content_peak from display_profile and
    // updates the surface image description if the peak changed.
    void updateContentPeak();

    // Not used — dmabuf path handles presentation directly
    bool startFrame(VkImage*, VkImageView*, VkFormat*) override { return false; }
    void submitFrame() override {}

    // Accessors
    wl_display* display() const { return wl_display_; }
    wl_surface* surface() const { return mpv_surface_; }
    VkFormat swapchainFormat() const override { return VK_FORMAT_UNDEFINED; }
    VkExtent2D swapchainExtent() const override { return swapchain_extent_; }
    bool isHdr() const override { return output_is_hdr_; }
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
    void setSwapchain(const void*) {}  // Mesa handles swapchain color on Wayland
    const mpv_display_profile& displayProfile() const { return display_profile_; }
    bool hasDmabufPool() const { return !dmabuf_pool_.empty(); }
    uint32_t dmabufWidth() const { return dmabuf_width_; }
    uint32_t dmabufHeight() const { return dmabuf_height_; }
    VkFormat dmabufFormat() const { return dmabuf_vk_format_; }


    void commit();
    void hide() override;
    void setColorspace() override {}
    void setDestinationSize(int width, int height) override;
    void initDestinationSize(int width, int height);

    static void registryGlobal(void* data, wl_registry* registry,
                               uint32_t name, const char* interface, uint32_t version);
    static void registryGlobalRemove(void* data, wl_registry* registry, uint32_t name);

    // Set up color feedback listener on the parent surface.
    void setupColorFeedback();

    // Called inline from preferred_changed — synchronous query + apply,
    // matching mpv's get_compositor_preferred_description pattern.
    void handlePreferredChanged();

    // Stub — preferred_changed is handled synchronously on SDL's main thread.
    // Kept because VulkanSubsurfaceRenderer calls it per-frame.
    void dispatchColorEvents();

    // Public for C listener callbacks (color manager capabilities)
    bool supports_parametric_ = false;
    bool supports_set_luminances_ = false;
    bool output_is_hdr_ = false;
    uint32_t transfer_map_[32] = {};
    uint32_t primaries_map_[32] = {};

private:
    bool initWayland(SDL_Window* window);
    bool createSubsurface(wl_surface* parentSurface);

    wl_display* wl_display_ = nullptr;
    wl_compositor* wl_compositor_ = nullptr;
    wl_subcompositor* wl_subcompositor_ = nullptr;
    wl_surface* parent_surface_ = nullptr;
    wl_surface* mpv_surface_ = nullptr;
    wl_subsurface* mpv_subsurface_ = nullptr;
    wl_surface* cef_surface_ = nullptr;
    wl_subsurface* cef_subsurface_ = nullptr;

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

    // Dmabuf buffer pool
    std::vector<DmabufBuffer> dmabuf_pool_;
    VkFormat dmabuf_vk_format_ = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    uint32_t dmabuf_drm_format_ = DRM_FORMAT_ABGR2101010;
    uint32_t dmabuf_width_ = 0;
    uint32_t dmabuf_height_ = 0;

    zwp_linux_dmabuf_v1* dmabuf_ = nullptr;
    wp_color_manager_v1* color_manager_ = nullptr;
    wp_color_management_surface_v1* color_surface_ = nullptr;
    wp_color_management_surface_feedback_v1* color_feedback_ = nullptr;
    wl_output* wl_output_ = nullptr;
    mpv_display_profile display_profile_ = {};
    struct pl_color_space preferred_csp_ = {};  // scaled preferred_csp, same as mpv's wl->preferred_csp
    bool preferred_csp_valid_ = false;

    // Last protocol-level values sent via setHdrImageDescription.
    // Deduplicates to break the preferred_changed → set_image_description loop.
    long last_sent_cll_ = -1;
    long last_sent_min_mastering_ = -1;
    long last_sent_max_mastering_ = -1;

    VkExtent2D swapchain_extent_ = {0, 0};

    std::atomic<int> pending_dest_width_{0};
    std::atomic<int> pending_dest_height_{0};
    std::atomic<bool> dest_pending_{false};
};
