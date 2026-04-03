#include "platform/wayland_subsurface.h"
#include <SDL3/SDL.h>
#include <libplacebo/colorspace.h>
#include "logging.h"
#include <cmath>     // lrintf, fabsf, fmaxf, fminf
#include <cstring>
#include <unistd.h>  // close()

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

// Async display profile query — mirrors mpv's vo_wayland_preferred_description_info.
// Accumulates all info events into a pl_color_space + raw luminances, then
// dispatchColorEvents applies mpv's info_done scaling to produce preferred_csp_.
#define WAYLAND_COLOR_FACTOR 1000000
#define WAYLAND_MIN_LUM_FACTOR 10000
struct DisplayProfileQuery {
    struct pl_color_space csp = {};   // target primaries/luminance go here
    float min_luma = 0;               // from info_luminances (raw, for scaling)
    float max_luma = 0;
    float ref_luma = 0;
    bool done = false;
};
static const struct wp_image_description_info_v1_listener s_profileInfoListener = {
    .done = [](void* d, struct wp_image_description_info_v1* info) {
        static_cast<DisplayProfileQuery*>(d)->done = true;
        wp_image_description_info_v1_destroy(info);
    },
    .icc_file = [](void*, struct wp_image_description_info_v1*, int32_t fd, uint32_t) { close(fd); },
    .primaries = [](void*, struct wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {},
    .primaries_named = [](void*, struct wp_image_description_info_v1*, uint32_t) {},
    .tf_power = [](void*, struct wp_image_description_info_v1*, uint32_t) {},
    .tf_named = [](void*, struct wp_image_description_info_v1*, uint32_t) {},
    // Same as mpv wayland_common.c info_luminances (line 2223)
    .luminances = [](void* d, struct wp_image_description_info_v1*, uint32_t min, uint32_t max, uint32_t ref) {
        auto* q = static_cast<DisplayProfileQuery*>(d);
        q->min_luma = min / (float)WAYLAND_MIN_LUM_FACTOR;
        q->max_luma = (float)max;
        q->ref_luma = (float)ref;
    },
    // Same as mpv wayland_common.c info_target_primaries (line 2232)
    .target_primaries = [](void* d, struct wp_image_description_info_v1*,
            int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y,
            int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y) {
        auto* q = static_cast<DisplayProfileQuery*>(d);
        q->csp.hdr.prim.red.x   = (float)r_x / WAYLAND_COLOR_FACTOR;
        q->csp.hdr.prim.red.y   = (float)r_y / WAYLAND_COLOR_FACTOR;
        q->csp.hdr.prim.green.x = (float)g_x / WAYLAND_COLOR_FACTOR;
        q->csp.hdr.prim.green.y = (float)g_y / WAYLAND_COLOR_FACTOR;
        q->csp.hdr.prim.blue.x  = (float)b_x / WAYLAND_COLOR_FACTOR;
        q->csp.hdr.prim.blue.y  = (float)b_y / WAYLAND_COLOR_FACTOR;
        q->csp.hdr.prim.white.x = (float)w_x / WAYLAND_COLOR_FACTOR;
        q->csp.hdr.prim.white.y = (float)w_y / WAYLAND_COLOR_FACTOR;
    },
    // Same as mpv wayland_common.c info_target_luminance (line 2247)
    .target_luminance = [](void* d, struct wp_image_description_info_v1*, uint32_t min, uint32_t max) {
        auto* q = static_cast<DisplayProfileQuery*>(d);
        q->csp.hdr.min_luma = (float)min / WAYLAND_MIN_LUM_FACTOR;
        q->csp.hdr.max_luma = (float)max;
    },
    // Same as mpv wayland_common.c info_target_max_cll/fall (lines 2255, 2261)
    .target_max_cll = [](void* d, struct wp_image_description_info_v1*, uint32_t v) {
        static_cast<DisplayProfileQuery*>(d)->csp.hdr.max_cll = (float)v;
    },
    .target_max_fall = [](void* d, struct wp_image_description_info_v1*, uint32_t v) {
        static_cast<DisplayProfileQuery*>(d)->csp.hdr.max_fall = (float)v;
    },
};

// Surface feedback listener — compositor notifies when preferred color changes.
// Handles the query synchronously inline (like mpv's preferred_changed handler).
static void preferred_changed(void* data, struct wp_color_management_surface_feedback_v1*, uint32_t) {
    static_cast<WaylandSubsurface*>(data)->handlePreferredChanged();
}
static void preferred_changed2(void* data, struct wp_color_management_surface_feedback_v1* f, uint32_t, uint32_t) {
    preferred_changed(data, f, 0);
}
static const struct wp_color_management_surface_feedback_v1_listener s_feedbackListener = {
    .preferred_changed = preferred_changed,
    .preferred_changed2 = preferred_changed2,
};

// Color manager listener — captures compositor HDR capabilities
static void cm_intent(void*, struct wp_color_manager_v1*, uint32_t) {}
static void cm_feature(void* data, struct wp_color_manager_v1*, uint32_t feature) {
    auto* s = static_cast<WaylandSubsurface*>(data);
    if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) s->supports_parametric_ = true;
    else if (feature == WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES) s->supports_set_luminances_ = true;
}
static void cm_tf(void* data, struct wp_color_manager_v1*, uint32_t tf) {
    auto* s = static_cast<WaylandSubsurface*>(data);
    int pl = -1;
    switch (tf) {
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB: pl = PL_COLOR_TRC_SRGB; break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ: pl = PL_COLOR_TRC_PQ; break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886: pl = PL_COLOR_TRC_BT_1886; break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22: pl = PL_COLOR_TRC_GAMMA22; break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR: pl = PL_COLOR_TRC_LINEAR; break;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG: pl = PL_COLOR_TRC_HLG; break;
    default: return;
    }
    if (pl >= 0 && pl < 32) s->transfer_map_[pl] = tf;
}
static void cm_prim(void* data, struct wp_color_manager_v1*, uint32_t prim) {
    auto* s = static_cast<WaylandSubsurface*>(data);
    int pl = -1;
    switch (prim) {
    case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB: pl = PL_COLOR_PRIM_BT_709; break;
    case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020: pl = PL_COLOR_PRIM_BT_2020; break;
    case WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3: pl = PL_COLOR_PRIM_DCI_P3; break;
    case WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3: pl = PL_COLOR_PRIM_DISPLAY_P3; break;
    default: return;
    }
    if (pl >= 0 && pl < 32) s->primaries_map_[pl] = prim;
}
static void cm_done(void*, struct wp_color_manager_v1*) {}
static const struct wp_color_manager_v1_listener s_cmListener = {
    .supported_intent = cm_intent,
    .supported_feature = cm_feature,
    .supported_tf_named = cm_tf,
    .supported_primaries_named = cm_prim,
    .done = cm_done,
};

static const wl_registry_listener s_registryListener = {
    .global = WaylandSubsurface::registryGlobal,
    .global_remove = WaylandSubsurface::registryGlobalRemove,
};

WaylandSubsurface::WaylandSubsurface() = default;
WaylandSubsurface::~WaylandSubsurface() { cleanup(); }

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
        wp_color_manager_v1_add_listener(self->color_manager_, &s_cmListener, self);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        self->dmabuf_ = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, std::min(version, 4u)));
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
    parent_surface_ = parent;

    wl_registry* reg = wl_display_get_registry(wl_display_);
    wl_registry_add_listener(reg, &s_registryListener, this);
    wl_display_roundtrip(wl_display_);
    // Second roundtrip receives color manager capability events
    // (supported_feature, supported_tf_named, supported_primaries_named)
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

    // Set up color feedback and do initial synchronous query to determine
    // HDR/SDR from the surface's preferred description (reflects EDR state).
    setupColorFeedback();

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

void WaylandSubsurface::setupColorFeedback() {
    if (color_feedback_ || !color_manager_ || !parent_surface_) return;

    // Feedback on the parent (top-level) surface. KWin only sends
    // preferred_changed to top-level surfaces with an active image
    // description — call activateColorManagement() from the dmabuf
    // init path to set the initial image description and start the
    // preferred_changed flow.
    color_feedback_ = wp_color_manager_v1_get_surface_feedback(color_manager_, parent_surface_);
    if (!color_feedback_) return;
    wp_color_management_surface_feedback_v1_add_listener(color_feedback_, &s_feedbackListener, this);

    // Synchronous initial query to determine HDR/SDR — same as mpv's
    // get_compositor_preferred_description at init. Uses the surface
    // feedback's preferred description which reflects EDR state (unlike
    // the output description which only reports base panel capabilities).
    handlePreferredChanged();
    output_is_hdr_ = preferred_csp_valid_ && preferred_csp_.hdr.max_luma > PL_COLOR_SDR_WHITE;
    LOG_INFO(LOG_PLATFORM, "Display: peak=%.1f → %s",
             preferred_csp_.hdr.max_luma, output_is_hdr_ ? "HDR" : "SDR");
}

void WaylandSubsurface::handlePreferredChanged() {
    if (!color_feedback_) return;

    // Synchronous query — same pattern as mpv's get_compositor_preferred_description.
    // Uses a temp queue so the blocking dispatch doesn't steal events from SDL.
    auto* desc = wp_color_management_surface_feedback_v1_get_preferred(color_feedback_);
    if (!desc) return;

    auto* info = wp_image_description_v1_get_information(desc);
    wp_image_description_v1_destroy(desc);
    if (!info) return;

    DisplayProfileQuery q{};
    auto* dq = wl_display_create_queue(wl_display_);
    wl_proxy_set_queue((wl_proxy*)info, dq);
    wp_image_description_info_v1_add_listener(info, &s_profileInfoListener, &q);
    while (wl_display_dispatch_queue(wl_display_, dq) > 0)
        if (q.done) break;
    wl_event_queue_destroy(dq);

    if (!q.done || q.ref_luma <= 0) return;

    // Apply mpv's info_done scaling — direct port of wayland_common.c:2136-2173.
    float a = q.min_luma;
    float b = (PL_COLOR_SDR_WHITE - PL_COLOR_HDR_BLACK) / (q.ref_luma - a);
    q.csp.hdr.min_luma = (q.csp.hdr.min_luma - a) * b + PL_COLOR_HDR_BLACK;
    q.csp.hdr.max_luma = (q.csp.hdr.max_luma - a) * b + PL_COLOR_HDR_BLACK;
    if (q.csp.hdr.max_cll != 0)
        q.csp.hdr.max_cll = (q.csp.hdr.max_cll - a) * b + PL_COLOR_HDR_BLACK;
    if (q.csp.hdr.max_fall != 0)
        q.csp.hdr.max_fall = (q.csp.hdr.max_fall - a) * b + PL_COLOR_HDR_BLACK;
    q.csp.hdr.min_luma = fmaxf(q.csp.hdr.min_luma, 0.0f);
    if (fabsf(q.csp.hdr.max_luma - PL_COLOR_SDR_WHITE) < 1e-2f) {
        q.csp.hdr.max_luma = PL_COLOR_SDR_WHITE;
        if (q.csp.hdr.max_cll != 0)
            q.csp.hdr.max_cll = fminf(q.csp.hdr.max_cll, q.csp.hdr.max_luma);
        if (q.csp.hdr.max_fall != 0)
            q.csp.hdr.max_fall = fminf(q.csp.hdr.max_fall, q.csp.hdr.max_luma);
    }

    bool changed = !pl_color_space_equal(&q.csp, &preferred_csp_);
    preferred_csp_ = q.csp;
    preferred_csp_valid_ = true;

    // Update display_profile_ for the render thread (video.c reads per-frame).
    // Only write non-zero values when we own color management (dmabuf path) —
    // on the swapchain path, non-zero values cause context.c to send a false
    // HDR hint to Mesa, which then sets PQ/BT.2020 on an SDR surface.
    if (color_surface_) {
        display_profile_.max_luma = q.csp.hdr.max_luma > 0
            ? (q.max_luma > 0 ? q.max_luma : q.ref_luma) : 0;
        display_profile_.min_luma = q.min_luma;
        display_profile_.ref_luma = q.ref_luma;
    }

    if (changed) {
        LOG_INFO(LOG_PLATFORM, "Display profile: scaled peak=%.1f min=%.4f (raw max=%.0f ref=%.0f)",
                 preferred_csp_.hdr.max_luma, preferred_csp_.hdr.min_luma,
                 q.max_luma, q.ref_luma);

        // Only update the surface image description when we own color
        // management (dmabuf path — color_surface_ set by activateColorManagement).
        // On the swapchain path, Mesa's WSI handles this via pl_swapchain.
        if (color_surface_ && preferred_csp_.hdr.max_luma > PL_COLOR_SDR_WHITE) {
            struct pl_hdr_metadata hdr = preferred_csp_.hdr;
            long cll = lrintf(hdr.max_cll);
            long min_m = pl_primaries_valid(&hdr.prim) ? lrintf(hdr.min_luma * WAYLAND_MIN_LUM_FACTOR) : 0;
            long max_m = pl_primaries_valid(&hdr.prim) ? lrintf(hdr.max_luma) : 0;
            if (cll != last_sent_cll_ || min_m != last_sent_min_mastering_ || max_m != last_sent_max_mastering_) {
                last_sent_cll_ = cll;
                last_sent_min_mastering_ = min_m;
                last_sent_max_mastering_ = max_m;
                setHdrImageDescription();
            }
        }
    }
}

// --- Dmabuf buffer pool ---

static void buffer_release(void* data, struct wl_buffer*) {
    auto* buf = static_cast<DmabufBuffer*>(data);
    buf->busy = false;
}
static const struct wl_buffer_listener s_bufferListener = { .release = buffer_release };

bool WaylandSubsurface::initDmabufPool(uint32_t width, uint32_t height) {
    if (!dmabuf_ || !device_) return false;

    destroyDmabufPool();
    dmabuf_width_ = width;
    dmabuf_height_ = height;

    // Load Vulkan function pointers for external memory
    auto vkGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
    if (!vkGetMemoryFdKHR) {
        LOG_ERROR(LOG_PLATFORM, "vkGetMemoryFdKHR not available");
        return false;
    }

    static constexpr int POOL_SIZE = 4;
    dmabuf_pool_.resize(POOL_SIZE);

    for (int i = 0; i < POOL_SIZE; i++) {
        auto& buf = dmabuf_pool_[i];

        // Create VkImage with external memory (dmabuf-exportable)
        VkExternalMemoryImageCreateInfo extMemInfo{};
        extMemInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        extMemInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.pNext = &extMemInfo;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = dmabuf_vk_format_;
        imgInfo.extent = {width, height, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_LINEAR;  // linear for dmabuf export
        imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device_, &imgInfo, nullptr, &buf.image) != VK_SUCCESS) {
            LOG_ERROR(LOG_PLATFORM, "Failed to create dmabuf VkImage %d", i);
            return false;
        }

        // Get memory requirements
        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device_, buf.image, &memReqs);

        // Find device-local memory type
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memProps);
        uint32_t memType = UINT32_MAX;
        for (uint32_t j = 0; j < memProps.memoryTypeCount; j++) {
            if ((memReqs.memoryTypeBits & (1 << j)) &&
                (memProps.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memType = j;
                break;
            }
        }
        if (memType == UINT32_MAX) {
            LOG_ERROR(LOG_PLATFORM, "No suitable memory type for dmabuf");
            return false;
        }

        // Allocate with export capability
        VkExportMemoryAllocateInfo exportInfo{};
        exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.pNext = &exportInfo;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = memType;

        if (vkAllocateMemory(device_, &allocInfo, nullptr, &buf.memory) != VK_SUCCESS) {
            LOG_ERROR(LOG_PLATFORM, "Failed to allocate dmabuf memory %d", i);
            return false;
        }
        vkBindImageMemory(device_, buf.image, buf.memory, 0);

        // Export dmabuf fd
        VkMemoryGetFdInfoKHR fdInfo{};
        fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fdInfo.memory = buf.memory;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        if (vkGetMemoryFdKHR(device_, &fdInfo, &buf.dmabuf_fd) != VK_SUCCESS) {
            LOG_ERROR(LOG_PLATFORM, "Failed to export dmabuf fd %d", i);
            return false;
        }

        // Get stride from subresource layout (linear tiling)
        VkSubresourceLayout layout;
        VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
        vkGetImageSubresourceLayout(device_, buf.image, &subres, &layout);
        buf.stride = (uint32_t)layout.rowPitch;

        // Create wl_buffer from dmabuf
        auto* params = zwp_linux_dmabuf_v1_create_params(dmabuf_);
        zwp_linux_buffer_params_v1_add(params, buf.dmabuf_fd, 0,
                                        (uint32_t)layout.offset, buf.stride,
                                        0, 0);  // no modifier (linear)
        buf.buffer = zwp_linux_buffer_params_v1_create_immed(
            params, width, height, dmabuf_drm_format_, 0);
        zwp_linux_buffer_params_v1_destroy(params);

        if (!buf.buffer) {
            LOG_ERROR(LOG_PLATFORM, "Failed to create wl_buffer from dmabuf %d", i);
            return false;
        }

        wl_buffer_add_listener(buf.buffer, &s_bufferListener, &buf);

        // Create VkImageView for FBO rendering
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = buf.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = dmabuf_vk_format_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device_, &viewInfo, nullptr, &buf.view);
    }

    LOG_INFO(LOG_PLATFORM, "Dmabuf pool created: %dx%d, %d buffers, format=0x%x",
             width, height, POOL_SIZE, dmabuf_drm_format_);
    return true;
}

DmabufBuffer* WaylandSubsurface::acquireBuffer() {
    for (auto& buf : dmabuf_pool_) {
        if (!buf.busy) return &buf;
    }
    return nullptr;  // all buffers in use by compositor
}

void WaylandSubsurface::presentBuffer(DmabufBuffer* buf) {
    if (!buf || !mpv_surface_) return;
    buf->busy = true;
    // Apply any pending viewport destination before commit — setDestinationSize
    // may arrive after recreateSwapchain already ran for this resize.
    if (viewport_ && dest_pending_.exchange(false, std::memory_order_acquire)) {
        wp_viewport_set_destination(viewport_,
            pending_dest_width_.load(std::memory_order_relaxed),
            pending_dest_height_.load(std::memory_order_relaxed));
    }
    wl_surface_attach(mpv_surface_, buf->buffer, 0, 0);
    wl_surface_damage_buffer(mpv_surface_, 0, 0, dmabuf_width_, dmabuf_height_);
    wl_surface_commit(mpv_surface_);
}

void WaylandSubsurface::dispatchColorEvents() {
    // No-op: preferred_changed is now handled synchronously inline by
    // handlePreferredChanged() on SDL's main thread. This method is kept
    // as a stub since VulkanSubsurfaceRenderer calls it per-frame.
}

void WaylandSubsurface::destroyDmabufPool() {
    for (auto& buf : dmabuf_pool_) {
        if (buf.view) vkDestroyImageView(device_, buf.view, nullptr);
        if (buf.image) vkDestroyImage(device_, buf.image, nullptr);
        if (buf.memory) vkFreeMemory(device_, buf.memory, nullptr);
        if (buf.buffer) wl_buffer_destroy(buf.buffer);
        if (buf.dmabuf_fd >= 0) close(buf.dmabuf_fd);
    }
    dmabuf_pool_.clear();
}

void WaylandSubsurface::activateColorManagement() {
    if (!color_manager_ || !parent_surface_) return;
    if (!color_surface_)
        color_surface_ = wp_color_manager_v1_get_surface(color_manager_, parent_surface_);
    // Set initial PQ/BT.2020 description so KWin starts sending
    // preferred_changed events for this surface.
    if (color_surface_)
        setHdrImageDescription();
}

void WaylandSubsurface::setHdrImageDescription(uint32_t, uint32_t) {
    uint32_t wl_prim = primaries_map_[PL_COLOR_PRIM_BT_2020];
    uint32_t wl_tf = transfer_map_[PL_COLOR_TRC_PQ];
    if (!supports_parametric_ || !wl_prim || !wl_tf || !color_manager_)
        return;

    if (!color_surface_) return;

    // Matches mpv wayland_common.c set_color_management (lines 3471-3535).
    // Uses preferred_csp_ (already scaled by info_done logic) and converts
    // to protocol values with lrintf — same conversions as mpv.
    auto* creator = wp_color_manager_v1_create_parametric_creator(color_manager_);
    wp_image_description_creator_params_v1_set_primaries_named(creator, wl_prim);
    wp_image_description_creator_params_v1_set_tf_named(creator, wl_tf);

    struct pl_hdr_metadata hdr = preferred_csp_.hdr;
    bool have_hdr = preferred_csp_valid_ && hdr.max_luma > PL_COLOR_SDR_WHITE;
    bool have_primaries = pl_primaries_valid(&hdr.prim);

    if (have_hdr) {
        // Same as mpv lines 3529-3531 + vo_gpu_next.c line 1105 (max_fall zeroed)
        wp_image_description_creator_params_v1_set_max_cll(creator, lrintf(hdr.max_cll));
        wp_image_description_creator_params_v1_set_max_fall(creator, 0);

        if (have_primaries) {
            // Same as mpv lines 3514-3522
            wp_image_description_creator_params_v1_set_mastering_display_primaries(
                creator,
                lrintf(hdr.prim.red.x   * WAYLAND_COLOR_FACTOR),
                lrintf(hdr.prim.red.y   * WAYLAND_COLOR_FACTOR),
                lrintf(hdr.prim.green.x * WAYLAND_COLOR_FACTOR),
                lrintf(hdr.prim.green.y * WAYLAND_COLOR_FACTOR),
                lrintf(hdr.prim.blue.x  * WAYLAND_COLOR_FACTOR),
                lrintf(hdr.prim.blue.y  * WAYLAND_COLOR_FACTOR),
                lrintf(hdr.prim.white.x * WAYLAND_COLOR_FACTOR),
                lrintf(hdr.prim.white.y * WAYLAND_COLOR_FACTOR));
            // Same as mpv lines 3526-3527
            wp_image_description_creator_params_v1_set_mastering_luminance(
                creator,
                lrintf(hdr.min_luma * WAYLAND_MIN_LUM_FACTOR),
                lrintf(hdr.max_luma));
        }
    }

    auto* img_desc = wp_image_description_creator_params_v1_create(creator);

    ImageDescContext dc{};
    auto* dq = wl_display_create_queue(wl_display_);
    wl_proxy_set_queue((wl_proxy*)img_desc, dq);
    wp_image_description_v1_add_listener(img_desc, &s_descListener, &dc);
    while (wl_display_dispatch_queue(wl_display_, dq) > 0)
        if (dc.ready) break;

    // Detach proxy from temp queue before destroying it — otherwise
    // wl_event_queue_destroy warns about proxies still attached.
    wl_proxy_set_queue((wl_proxy*)img_desc, nullptr);
    wl_event_queue_destroy(dq);

    if (dc.ready) {
        wp_color_management_surface_v1_set_image_description(
            color_surface_, img_desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        wl_display_flush(wl_display_);
        LOG_INFO(LOG_PLATFORM, "Set HDR image description: PQ/BT.2020 cll=%ld mastering=(%ld,%ld)",
                 have_hdr ? lrintf(hdr.max_cll) : 0L,
                 have_hdr && have_primaries ? lrintf(hdr.min_luma * WAYLAND_MIN_LUM_FACTOR) : 0L,
                 have_hdr && have_primaries ? lrintf(hdr.max_luma) : 0L);
    }
    wp_image_description_v1_destroy(img_desc);
}

void WaylandSubsurface::updateContentPeak() {
    // No-op: image description is now updated from dispatchColorEvents
    // when the compositor's preferred_csp changes.
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
    destroyDmabufPool();
    if (color_feedback_) { wp_color_management_surface_feedback_v1_destroy(color_feedback_); color_feedback_ = nullptr; }
    if (color_surface_) { wp_color_management_surface_v1_destroy(color_surface_); color_surface_ = nullptr; }
    if (dmabuf_) { zwp_linux_dmabuf_v1_destroy(dmabuf_); dmabuf_ = nullptr; }
    if (color_manager_) { wp_color_manager_v1_destroy(color_manager_); color_manager_ = nullptr; }
    if (wl_output_) { wl_output_destroy(wl_output_); wl_output_ = nullptr; }
    if (vk_surface_ && instance_) { vkDestroySurfaceKHR(instance_, vk_surface_, nullptr); vk_surface_ = VK_NULL_HANDLE; }
    if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
    if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    if (viewport_) { wp_viewport_destroy(viewport_); viewport_ = nullptr; }
    if (viewporter_) { wp_viewporter_destroy(viewporter_); viewporter_ = nullptr; }
    if (cef_subsurface_) { wl_subsurface_destroy(cef_subsurface_); cef_subsurface_ = nullptr; }
    if (cef_surface_) { wl_surface_destroy(cef_surface_); cef_surface_ = nullptr; }
    if (mpv_subsurface_) { wl_subsurface_destroy(mpv_subsurface_); mpv_subsurface_ = nullptr; }
    if (mpv_surface_) { wl_surface_destroy(mpv_surface_); mpv_surface_ = nullptr; }
    wl_compositor_ = nullptr; wl_subcompositor_ = nullptr; wl_display_ = nullptr;
}

VkQueue WaylandSubsurface::vkQueue() const { return queue_; }
uint32_t WaylandSubsurface::vkQueueFamily() const { return queue_family_; }
const char* const* WaylandSubsurface::deviceExtensions() const { return enabled_extensions_.data(); }
int WaylandSubsurface::deviceExtensionCount() const { return (int)enabled_extensions_.size(); }
