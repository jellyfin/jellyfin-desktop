#include "platform/wayland_subsurface.h"
#include <SDL3/SDL.h>
#include "logging.h"
#include <cstring>

static const char* s_requiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
};

static const char* s_optionalDeviceExtensions[] = {
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_HDR_METADATA_EXTENSION_NAME,
};

// Image description listener for display profile query
struct ImageDescContext { bool ready = false; };
static void desc_failed(void*, struct wp_image_description_v1*, uint32_t, const char*) {}
static void desc_ready(void* d, struct wp_image_description_v1*, uint32_t) { ((ImageDescContext*)d)->ready = true; }
static void desc_ready2(void* d, struct wp_image_description_v1*, uint32_t, uint32_t) { ((ImageDescContext*)d)->ready = true; }
static const struct wp_image_description_v1_listener s_descListener = {
    .failed = desc_failed, .ready = desc_ready, .ready2 = desc_ready2,
};

// Output info listener
struct OutputInfoCtx { bool done = false; uint32_t max_lum = 0; uint32_t min_lum = 0; uint32_t ref_lum = 0; };
static const struct wp_image_description_info_v1_listener s_infoListener = {
    .done = [](void* d, struct wp_image_description_info_v1*) { ((OutputInfoCtx*)d)->done = true; },
    .icc_file = [](void*, struct wp_image_description_info_v1*, int32_t, uint32_t) {},
    .primaries = [](void*, struct wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {},
    .primaries_named = [](void*, struct wp_image_description_info_v1*, uint32_t) {},
    .tf_power = [](void*, struct wp_image_description_info_v1*, uint32_t) {},
    .tf_named = [](void*, struct wp_image_description_info_v1*, uint32_t) {},
    .luminances = [](void* d, struct wp_image_description_info_v1*, uint32_t min, uint32_t max, uint32_t ref) {
        auto* c = (OutputInfoCtx*)d; c->min_lum = min; c->max_lum = max; c->ref_lum = ref;
    },
    .target_primaries = [](void*, struct wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {},
    .target_luminance = [](void*, struct wp_image_description_info_v1*, uint32_t, uint32_t) {},
    .target_max_cll = [](void*, struct wp_image_description_info_v1*, uint32_t) {},
    .target_max_fall = [](void*, struct wp_image_description_info_v1*, uint32_t) {},
};

static const wl_registry_listener s_registryListener = {
    .global = WaylandSubsurface::registryGlobal,
    .global_remove = WaylandSubsurface::registryGlobalRemove,
};

WaylandSubsurface::WaylandSubsurface() = default;
WaylandSubsurface::~WaylandSubsurface() { WaylandSubsurface::cleanup(); }

void WaylandSubsurface::registryGlobal(void* data, wl_registry* registry,
                                        uint32_t name, const char* interface, uint32_t version) {
    auto* self = static_cast<WaylandSubsurface*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        self->wl_compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        self->wl_subcompositor_ = static_cast<wl_subcompositor*>(
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
        self->color_manager_ = static_cast<wp_color_manager_v1*>(
            wl_registry_bind(registry, name, &wp_color_manager_v1_interface, std::min(version, 1u)));
    } else if (strcmp(interface, wl_output_interface.name) == 0 && !self->wl_output_) {
        self->wl_output_ = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, 1));
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        self->viewporter_ = static_cast<wp_viewporter*>(
            wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    }
}

void WaylandSubsurface::registryGlobalRemove(void*, wl_registry*, uint32_t) {}

bool WaylandSubsurface::initWayland(SDL_Window* window) {
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props) return false;

    wl_display_ = static_cast<wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    wl_surface* parent = static_cast<wl_surface*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    if (!wl_display_ || !parent) return false;

    wl_registry* reg = wl_display_get_registry(wl_display_);
    wl_registry_add_listener(reg, &s_registryListener, this);
    wl_display_roundtrip(wl_display_);
    wl_registry_destroy(reg);

    if (!wl_compositor_ || !wl_subcompositor_) return false;
    return createSubsurface(parent);
}

bool WaylandSubsurface::createSubsurface(wl_surface* parent) {
    mpv_surface_ = wl_compositor_create_surface(wl_compositor_);
    if (!mpv_surface_) return false;

    mpv_subsurface_ = wl_subcompositor_get_subsurface(wl_subcompositor_, mpv_surface_, parent);
    if (!mpv_subsurface_) return false;

    wl_subsurface_set_position(mpv_subsurface_, 0, 0);
    wl_subsurface_place_below(mpv_subsurface_, parent);
    wl_subsurface_set_desync(mpv_subsurface_);

    wl_region* empty = wl_compositor_create_region(wl_compositor_);
    wl_surface_set_input_region(mpv_surface_, empty);
    wl_region_destroy(empty);

    if (viewporter_)
        viewport_ = wp_viewporter_get_viewport(viewporter_, mpv_surface_);

    wl_surface_commit(mpv_surface_);
    wl_display_roundtrip(wl_display_);
    return true;
}

bool WaylandSubsurface::init(SDL_Window* window, VkInstance, VkPhysicalDevice,
                              VkDevice, uint32_t, const char* const*, int,
                              const VkPhysicalDeviceFeatures2*) {
    if (!initWayland(window)) return false;

    // Query display HDR profile (for mpv's libplacebo rendering target)
    queryDisplayProfile();

    // No color management surface — Mesa creates one via the swapchain.
    // This matches standalone mpv where only Mesa's WSI handles color management.

    const char* instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instInfo{};
    instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instInfo.pApplicationInfo = &appInfo;
    instInfo.enabledExtensionCount = 5;
    instInfo.ppEnabledExtensionNames = instanceExts;

    if (vkCreateInstance(&instInfo, nullptr, &instance_) != VK_SUCCESS) return false;

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr);
    if (!gpuCount) return false;
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance_, &gpuCount, gpus.data());
    physical_device_ = gpus[0];

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> avail(extCount);
    vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &extCount, avail.data());

    auto has = [&](const char* n) {
        for (auto& e : avail) if (strcmp(e.extensionName, n) == 0) return true;
        return false;
    };

    enabled_extensions_.clear();
    for (auto& e : s_requiredDeviceExtensions) {
        if (!has(e)) { LOG_ERROR(LOG_PLATFORM, "Missing: %s", e); return false; }
        enabled_extensions_.push_back(e);
    }
    for (auto& e : s_optionalDeviceExtensions)
        if (has(e)) enabled_extensions_.push_back(e);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qfCount, qfs.data());
    for (uint32_t i = 0; i < qfCount; i++)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queue_family_ = i; break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = queue_family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    vk11_features_ = {}; vk11_features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vk11_features_.samplerYcbcrConversion = VK_TRUE;
    vk12_features_ = {}; vk12_features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk12_features_.pNext = &vk11_features_;
    vk12_features_.timelineSemaphore = VK_TRUE;
    vk12_features_.hostQueryReset = VK_TRUE;
    features2_ = {}; features2_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2_.pNext = &vk12_features_;

    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.pNext = &features2_;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = (uint32_t)enabled_extensions_.size();
    di.ppEnabledExtensionNames = enabled_extensions_.data();

    if (vkCreateDevice(physical_device_, &di, nullptr, &device_) != VK_SUCCESS) return false;
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkWaylandSurfaceCreateInfoKHR si{};
    si.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    si.display = wl_display_;
    si.surface = mpv_surface_;
    auto fn = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
        vkGetInstanceProcAddr(instance_, "vkCreateWaylandSurfaceKHR"));
    if (!fn || fn(instance_, &si, nullptr, &vk_surface_) != VK_SUCCESS) return false;

    LOG_INFO(LOG_PLATFORM, "Vulkan subsurface initialized (libplacebo swapchain mode)");
    return true;
}

bool WaylandSubsurface::createSwapchain(int width, int height) {
    swapchain_extent_ = {(uint32_t)width, (uint32_t)height};
    return true;
}

bool WaylandSubsurface::recreateSwapchain(int width, int height) {
    swapchain_extent_ = {(uint32_t)width, (uint32_t)height};
    if (viewport_ && dest_pending_.exchange(false, std::memory_order_acquire)) {
        wp_viewport_set_destination(viewport_,
            pending_dest_width_.load(std::memory_order_relaxed),
            pending_dest_height_.load(std::memory_order_relaxed));
    }
    return true;
}

void WaylandSubsurface::queryDisplayProfile() {
    if (!color_manager_ || !wl_output_) return;

    auto* cm_out = wp_color_manager_v1_get_output(color_manager_, wl_output_);
    if (!cm_out) return;

    auto* desc = wp_color_management_output_v1_get_image_description(cm_out);
    if (!desc) { wp_color_management_output_v1_destroy(cm_out); return; }

    ImageDescContext dc{};
    wp_image_description_v1_add_listener(desc, &s_descListener, &dc);
    wl_display_roundtrip(wl_display_);
    if (!dc.ready) { wp_image_description_v1_destroy(desc); wp_color_management_output_v1_destroy(cm_out); return; }

    auto* info = wp_image_description_v1_get_information(desc);
    if (!info) { wp_image_description_v1_destroy(desc); wp_color_management_output_v1_destroy(cm_out); return; }

    OutputInfoCtx ic{};
    wp_image_description_info_v1_add_listener(info, &s_infoListener, &ic);
    wl_display_roundtrip(wl_display_);

    if (ic.max_lum > 0 && ic.ref_lum > 0) {
        display_profile_.max_luma = (float)ic.max_lum;
        display_profile_.min_luma = (float)ic.min_lum / 10000.0f;
        display_profile_.ref_luma = (float)ic.ref_lum;

        LOG_INFO(LOG_PLATFORM, "Display: max=%.0f min=%.4f ref=%.0f nits",
                 display_profile_.max_luma, display_profile_.min_luma, display_profile_.ref_luma);
    }

    wp_image_description_v1_destroy(desc);
    wp_color_management_output_v1_destroy(cm_out);
}

void WaylandSubsurface::commit() {
    wl_surface_commit(mpv_surface_);
    wl_display_flush(wl_display_);
}

void WaylandSubsurface::hide() {
    if (!mpv_surface_) return;
    wl_surface_attach(mpv_surface_, nullptr, 0, 0);
    wl_surface_commit(mpv_surface_);
    wl_display_flush(wl_display_);
}

void WaylandSubsurface::initDestinationSize(int w, int h) {
    if (viewport_ && w > 0 && h > 0)
        wp_viewport_set_destination(viewport_, w, h);
}

void WaylandSubsurface::setDestinationSize(int w, int h) {
    if (viewport_ && w > 0 && h > 0) {
        pending_dest_width_.store(w, std::memory_order_relaxed);
        pending_dest_height_.store(h, std::memory_order_relaxed);
        dest_pending_.store(true, std::memory_order_release);
    }
}

void WaylandSubsurface::cleanup() {
    if (color_manager_) { wp_color_manager_v1_destroy(color_manager_); color_manager_ = nullptr; }
    if (wl_output_) { wl_output_destroy(wl_output_); wl_output_ = nullptr; }
    if (vk_surface_ && instance_) { vkDestroySurfaceKHR(instance_, vk_surface_, nullptr); vk_surface_ = VK_NULL_HANDLE; }
    if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
    if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    if (viewport_) { wp_viewport_destroy(viewport_); viewport_ = nullptr; }
    if (viewporter_) { wp_viewporter_destroy(viewporter_); viewporter_ = nullptr; }
    if (mpv_subsurface_) { wl_subsurface_destroy(mpv_subsurface_); mpv_subsurface_ = nullptr; }
    if (mpv_surface_) { wl_surface_destroy(mpv_surface_); mpv_surface_ = nullptr; }
    wl_compositor_ = nullptr; wl_subcompositor_ = nullptr; wl_display_ = nullptr;
}

VkQueue WaylandSubsurface::vkQueue() const { return queue_; }
uint32_t WaylandSubsurface::vkQueueFamily() const { return queue_family_; }
const char* const* WaylandSubsurface::deviceExtensions() const { return enabled_extensions_.data(); }
int WaylandSubsurface::deviceExtensionCount() const { return (int)enabled_extensions_.size(); }
