#if defined(__linux__) && !defined(__ANDROID__)

#include "platform/x11_video_layer.h"
#include <SDL3/SDL.h>
#include "logging.h"
#include <algorithm>
#include <cstring>

// Required device extensions for mpv/libplacebo (same as Wayland)
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

// Optional extensions (dmabuf hwdec interop)
static const char* s_optionalDeviceExtensions[] = {
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
};

X11VideoLayer::X11VideoLayer() = default;

X11VideoLayer::~X11VideoLayer() {
    cleanup();
}

bool X11VideoLayer::initX11(SDL_Window* window) {
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props) {
        LOG_ERROR(LOG_PLATFORM, "[X11VideoLayer] Failed to get window properties");
        return false;
    }

    display_ = static_cast<Display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    parent_window_ = static_cast<Window>(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));

    if (!display_ || !parent_window_) {
        LOG_ERROR(LOG_PLATFORM, "[X11VideoLayer] Not running on X11 or failed to get X11 handles");
        return false;
    }

    // Get parent window dimensions
    XWindowAttributes attrs;
    XGetWindowAttributes(display_, parent_window_, &attrs);

    // Create child window for video (will be positioned behind CEF content)
    video_window_ = XCreateSimpleWindow(
        display_, parent_window_,
        0, 0, attrs.width, attrs.height,
        0,  // border width
        0,  // border color
        0   // background color (black)
    );

    if (!video_window_) {
        LOG_ERROR(LOG_PLATFORM, "[X11VideoLayer] Failed to create video child window");
        return false;
    }

    // Position at bottom of stacking order (below CEF content)
    XLowerWindow(display_, video_window_);
    XMapWindow(display_, video_window_);
    XFlush(display_);

    LOG_INFO(LOG_PLATFORM, "[X11VideoLayer] Created video child window: %dx%d", attrs.width, attrs.height);
    return true;
}

bool X11VideoLayer::init(SDL_Window* window, VkInstance, VkPhysicalDevice,
                          VkDevice, uint32_t,
                          const char* const*, int,
                          const VkPhysicalDeviceFeatures2*) {
    // We ignore passed-in Vulkan handles and create our own (like WaylandSubsurface)

    if (!initX11(window)) return false;

    // Create Vulkan instance with X11 surface extension
    const char* instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_3;
    appInfo.pApplicationName = "Jellyfin Desktop";

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 4;
    instanceInfo.ppEnabledExtensionNames = instanceExts;

    if (vkCreateInstance(&instanceInfo, nullptr, &instance_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[X11VideoLayer] Failed to create Vulkan instance");
        return false;
    }

    // Select physical device
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG_ERROR(LOG_PLATFORM, "[X11VideoLayer] No Vulkan devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance_, &gpuCount, gpus.data());
    physical_device_ = gpus[0];

    VkPhysicalDeviceProperties gpuProps;
    vkGetPhysicalDeviceProperties(physical_device_, &gpuProps);
    LOG_INFO(LOG_PLATFORM, "[X11VideoLayer] Using GPU: %s", gpuProps.deviceName);

    // Check for required extensions
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &extCount, availableExts.data());

    auto hasExtension = [&](const char* name) {
        for (const auto& ext : availableExts) {
            if (strcmp(ext.extensionName, name) == 0) return true;
        }
        return false;
    };

    enabled_extensions_.clear();
    constexpr int requiredCount = sizeof(s_requiredDeviceExtensions) / sizeof(s_requiredDeviceExtensions[0]);
    constexpr int optionalCount = sizeof(s_optionalDeviceExtensions) / sizeof(s_optionalDeviceExtensions[0]);

    for (int i = 0; i < requiredCount; i++) {
        if (!hasExtension(s_requiredDeviceExtensions[i])) {
            LOG_ERROR(LOG_PLATFORM, "[X11VideoLayer] Missing required extension: %s", s_requiredDeviceExtensions[i]);
            return false;
        }
        enabled_extensions_.push_back(s_requiredDeviceExtensions[i]);
    }

    for (int i = 0; i < optionalCount; i++) {
        if (hasExtension(s_optionalDeviceExtensions[i])) {
            enabled_extensions_.push_back(s_optionalDeviceExtensions[i]);
            LOG_INFO(LOG_PLATFORM, "[X11VideoLayer] Enabled optional extension: %s", s_optionalDeviceExtensions[i]);
        }
    }

    // Find graphics queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queue_family_ = i;
            break;
        }
    }

    // Create device with features needed for mpv/libplacebo
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queue_family_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    vk11_features_ = {};
    vk11_features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vk11_features_.samplerYcbcrConversion = VK_TRUE;

    vk12_features_ = {};
    vk12_features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk12_features_.pNext = &vk11_features_;
    vk12_features_.timelineSemaphore = VK_TRUE;
    vk12_features_.hostQueryReset = VK_TRUE;

    features2_ = {};
    features2_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2_.pNext = &vk12_features_;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &features2_;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions_.size());
    deviceInfo.ppEnabledExtensionNames = enabled_extensions_.data();

    VkResult deviceResult = vkCreateDevice(physical_device_, &deviceInfo, nullptr, &device_);
    if (deviceResult != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[X11VideoLayer] Failed to create Vulkan device: VkResult=%d", deviceResult);
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    // Create VkSurface for our X11 window
    VkXlibSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.dpy = display_;
    surfaceInfo.window = video_window_;

    auto vkCreateXlibSurfaceKHR = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(
        vkGetInstanceProcAddr(instance_, "vkCreateXlibSurfaceKHR"));
    if (!vkCreateXlibSurfaceKHR ||
        vkCreateXlibSurfaceKHR(instance_, &surfaceInfo, nullptr, &vk_surface_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[X11VideoLayer] Failed to create Vulkan X11 surface");
        return false;
    }

    LOG_INFO(LOG_PLATFORM, "[X11VideoLayer] Vulkan context initialized");
    return true;
}

void X11VideoLayer::destroySwapchain() {
    if (!device_) return;

    vkDeviceWaitIdle(device_);

    if (acquire_fence_) {
        vkDestroyFence(device_, acquire_fence_, nullptr);
        acquire_fence_ = VK_NULL_HANDLE;
    }
    if (image_available_) {
        vkDestroySemaphore(device_, image_available_, nullptr);
        image_available_ = VK_NULL_HANDLE;
    }

    for (auto view : swapchain_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_views_.clear();
    swapchain_images_.clear();

    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    frame_active_ = false;
}

void X11VideoLayer::resize(int width, int height) {
    if (video_window_ && display_) {
        XResizeWindow(display_, video_window_, width, height);
        XLowerWindow(display_, video_window_);
        XFlush(display_);
    }
}

void X11VideoLayer::cleanup() {
    destroySwapchain();

    if (vk_surface_ && instance_) {
        vkDestroySurfaceKHR(instance_, vk_surface_, nullptr);
        vk_surface_ = VK_NULL_HANDLE;
    }
    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    if (video_window_ && display_) {
        XDestroyWindow(display_, video_window_);
        video_window_ = 0;
    }

    // Note: display_ is owned by SDL, don't close it
    display_ = nullptr;
}
const char* const* X11VideoLayer::deviceExtensions() const { return enabled_extensions_.data(); }
int X11VideoLayer::deviceExtensionCount() const { return static_cast<int>(enabled_extensions_.size()); }

bool X11VideoLayer::createSwapchain(int width, int height) {
    // Query surface formats
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, vk_surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, vk_surface_, &formatCount, formats.data());

    // X11 doesn't have standard HDR support, use SDR format
    swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    // Find matching format
    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_UNORM &&
            fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            swapchain_format_ = fmt.format;
            colorSpace = fmt.colorSpace;
            break;
        }
    }

    // Get surface capabilities
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, vk_surface_, &caps);

    swapchain_extent_ = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    // Create swapchain
    VkSwapchainCreateInfoKHR swapInfo{};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = vk_surface_;
    swapInfo.minImageCount = caps.minImageCount + 1;
    swapInfo.imageFormat = swapchain_format_;
    swapInfo.imageColorSpace = colorSpace;
    swapInfo.imageExtent = swapchain_extent_;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &swapInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[X11VideoLayer] Failed to create swapchain");
        return false;
    }

    // Get swapchain images
    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchain_images_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchain_images_.data());

    // Create image views
    swapchain_views_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain_images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchain_format_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device_, &viewInfo, nullptr, &swapchain_views_[i]);
    }

    // Create sync objects
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(device_, &semInfo, nullptr, &image_available_);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(device_, &fenceInfo, nullptr, &acquire_fence_);

    LOG_INFO(LOG_PLATFORM, "[X11VideoLayer] Swapchain created: %dx%d format=%d", width, height, swapchain_format_);

    return true;
}

bool X11VideoLayer::startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat) {
    if (!swapchain_) return false;

    vkResetFences(device_, 1, &acquire_fence_);
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, 100000000,
                                            VK_NULL_HANDLE, acquire_fence_, &current_image_idx_);
    if (result == VK_TIMEOUT || result == VK_NOT_READY) {
        return false;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return false;
    }
    vkWaitForFences(device_, 1, &acquire_fence_, VK_TRUE, UINT64_MAX);

    frame_active_ = true;
    *outImage = swapchain_images_[current_image_idx_];
    *outView = swapchain_views_[current_image_idx_];
    *outFormat = swapchain_format_;
    return true;
}

void X11VideoLayer::submitFrame() {
    if (!frame_active_ || !swapchain_) return;

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &current_image_idx_;

    vkQueuePresentKHR(queue_, &presentInfo);

    frame_active_ = false;
}

bool X11VideoLayer::recreateSwapchain(int width, int height) {
    destroySwapchain();
    return createSwapchain(width, height);
}

#endif  // defined(__linux__) && !defined(__ANDROID__)
