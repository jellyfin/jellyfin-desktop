#include "context/vulkan_context.h"
#include "logging.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>

const char* VulkanContext::device_extensions_[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_HDR_METADATA_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
};

const int VulkanContext::device_extension_count_ = sizeof(device_extensions_) / sizeof(device_extensions_[0]);

VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext() {
    cleanup();
}

bool VulkanContext::init(SDL_Window* window) {
    if (!createInstance(window)) return false;
    if (!createSurface(window)) return false;
    if (!selectPhysicalDevice()) return false;
    if (!createDevice()) return false;
    if (!createCommandPool()) return false;
    return true;
}

bool VulkanContext::createInstance(SDL_Window* window) {
    // Get required extensions from SDL3
    Uint32 ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    std::vector<const char*> extensions(sdl_exts, sdl_exts + ext_count);

    // Add HDR colorspace extension
    extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Jellyfin Desktop";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "No Engine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    VkResult result = vkCreateInstance(&create_info, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        LOG_ERROR(LOG_MPV, "Failed to create Vulkan instance: %d", result);
        return false;
    }
    return true;
}

bool VulkanContext::createSurface(SDL_Window* window) {
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
        LOG_ERROR(LOG_MPV, "Failed to create Vulkan surface: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool VulkanContext::selectPhysicalDevice() {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) {
        LOG_ERROR(LOG_MPV, "No Vulkan devices found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
    physical_device_ = devices[0];  // Use first device

    // Find graphics queue family with present support
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, queue_families.data());

    for (uint32_t i = 0; i < queue_family_count; i++) {
        VkBool32 present_support = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &present_support);
        if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support) {
            queue_family_ = i;
            break;
        }
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    LOG_INFO(LOG_MPV, "Using GPU: %s", props.deviceName);

    return true;
}

bool VulkanContext::createDevice() {
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

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
    // Required by libplacebo
    features2_.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    features2_.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &features2_;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = device_extension_count_;
    device_info.ppEnabledExtensionNames = device_extensions_;

    VkResult result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
    if (result != VK_SUCCESS) {
        LOG_ERROR(LOG_MPV, "Failed to create Vulkan device: %d", result);
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
}

bool VulkanContext::createCommandPool() {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;

    VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_);
    if (result != VK_SUCCESS) {
        LOG_ERROR(LOG_MPV, "Failed to create command pool: %d", result);
        return false;
    }
    return true;
}

bool VulkanContext::createSwapchain(int width, int height) {
    LOG_INFO(LOG_MPV, "VulkanContext::createSwapchain called");
    VkSurfaceCapabilitiesKHR caps;
    VkResult caps_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
    if (caps_result != VK_SUCCESS) {
        LOG_ERROR(LOG_MPV, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: %d", caps_result);
    }

    uint32_t format_count = 0;
    VkResult fmt_result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
    LOG_INFO(LOG_MPV, "Surface format query: result=%d count=%u", fmt_result, format_count);
    if (format_count == 0) {
        LOG_ERROR(LOG_MPV, "No surface formats available");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());

    // Debug: print available formats
    LOG_INFO(LOG_MPV, "Available surface formats:");
    for (const auto& fmt : formats) {
        LOG_INFO(LOG_MPV, "  format=%d colorSpace=%d", fmt.format, fmt.colorSpace);
    }

    // SDR for main window (CEF overlay) - mpv uses separate HDR subsurface
    swapchain_format_ = formats[0].format;
    swapchain_color_space_ = formats[0].colorSpace;
    is_hdr_ = false;

    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_UNORM && fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            swapchain_format_ = fmt.format;
            swapchain_color_space_ = fmt.colorSpace;
            break;
        }
    }

    swapchain_extent_ = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    swapchain_extent_.width = std::clamp(swapchain_extent_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    swapchain_extent_.height = std::clamp(swapchain_extent_.height, caps.minImageExtent.height, caps.maxImageExtent.height);

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchain_info{};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = surface_;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = swapchain_format_;
    swapchain_info.imageColorSpace = swapchain_color_space_;
    swapchain_info.imageExtent = swapchain_extent_;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = caps.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;

    VkResult result = vkCreateSwapchainKHR(device_, &swapchain_info, nullptr, &swapchain_);
    if (result != VK_SUCCESS) {
        LOG_ERROR(LOG_MPV, "Failed to create swapchain: %d", result);
        return false;
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());

    swapchain_views_.resize(image_count);
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = swapchain_images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = swapchain_format_;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device_, &view_info, nullptr, &swapchain_views_[i]);
    }

    LOG_INFO(LOG_MPV, "Swapchain created: %ux%u (HDR: %s)", swapchain_extent_.width, swapchain_extent_.height, is_hdr_ ? "yes" : "no");

    if (is_hdr_) {
        setHdrMetadata();
    }

    return true;
}

void VulkanContext::setHdrMetadata() {
    auto vkSetHdrMetadataEXT = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
        vkGetDeviceProcAddr(device_, "vkSetHdrMetadataEXT"));
    if (!vkSetHdrMetadataEXT) {
        LOG_INFO(LOG_MPV, "vkSetHdrMetadataEXT not available");
        return;
    }

    VkHdrMetadataEXT hdr_metadata{};
    hdr_metadata.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;

    // BT.2020 primaries
    hdr_metadata.displayPrimaryRed = {0.708f, 0.292f};
    hdr_metadata.displayPrimaryGreen = {0.170f, 0.797f};
    hdr_metadata.displayPrimaryBlue = {0.131f, 0.046f};
    hdr_metadata.whitePoint = {0.3127f, 0.3290f};  // D65

    // Luminance range
    hdr_metadata.maxLuminance = 1000.0f;
    hdr_metadata.minLuminance = 0.001f;

    // Content light level
    hdr_metadata.maxContentLightLevel = 1000.0f;
    hdr_metadata.maxFrameAverageLightLevel = 200.0f;

    vkSetHdrMetadataEXT(device_, 1, &swapchain_, &hdr_metadata);
    LOG_INFO(LOG_MPV, "HDR metadata set");
}

bool VulkanContext::recreateSwapchain(int width, int height) {
    vkDeviceWaitIdle(device_);
    destroySwapchain();
    return createSwapchain(width, height);
}

void VulkanContext::destroySwapchain() {
    for (auto view : swapchain_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_views_.clear();
    swapchain_images_.clear();

    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

uint32_t VulkanContext::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

VkCommandBuffer VulkanContext::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = command_pool_;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    return cmd;
}

void VulkanContext::endSingleTimeCommands(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(device_, &fence_info, nullptr, &fence);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    vkQueueSubmit(queue_, 1, &submit_info, fence);
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
}

void VulkanContext::cleanup() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
    }

    if (command_pool_) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }

    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (surface_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}
