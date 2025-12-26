#ifdef USE_WAYLAND_SUBSURFACE

#include "WaylandVulkanContext.h"
#include <QDebug>
#include <algorithm>

const char* WaylandVulkanContext::m_deviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_EXT_HDR_METADATA_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
};

const int WaylandVulkanContext::m_deviceExtensionCount = sizeof(m_deviceExtensions) / sizeof(m_deviceExtensions[0]);

WaylandVulkanContext::WaylandVulkanContext()
{
}

WaylandVulkanContext::~WaylandVulkanContext()
{
    cleanup();
}

bool WaylandVulkanContext::initialize(wl_display *display, wl_surface *wlSurface)
{
    if (!createInstance())
        return false;
    if (!selectPhysicalDevice())
        return false;
    if (!createDevice())
        return false;
    if (!createVulkanSurface(display, wlSurface))
        return false;
    return true;
}

bool WaylandVulkanContext::createInstance()
{
    const char *instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.apiVersion = VK_API_VERSION_1_3;
    appInfo.pApplicationName = "Jellyfin Desktop";

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 5;
    instanceInfo.ppEnabledExtensionNames = instanceExts;

    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &m_instance);
    if (result != VK_SUCCESS) {
        qCritical() << "Failed to create Vulkan instance:" << result;
        return false;
    }
    return true;
}

bool WaylandVulkanContext::selectPhysicalDevice()
{
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &gpuCount, nullptr);
    if (gpuCount == 0) {
        qCritical() << "No Vulkan devices found";
        return false;
    }

    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(m_instance, &gpuCount, gpus.data());
    m_physicalDevice = gpus[0];

    // Find graphics queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_queueFamily = i;
            break;
        }
    }

    return true;
}

bool WaylandVulkanContext::createDevice()
{
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = m_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    m_vk11Features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    m_vk11Features.samplerYcbcrConversion = VK_TRUE;

    m_vk12Features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    m_vk12Features.pNext = &m_vk11Features;
    m_vk12Features.timelineSemaphore = VK_TRUE;
    m_vk12Features.hostQueryReset = VK_TRUE;

    m_features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    m_features2.pNext = &m_vk12Features;

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &m_features2;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = m_deviceExtensionCount;
    deviceInfo.ppEnabledExtensionNames = m_deviceExtensions;

    VkResult result = vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device);
    if (result != VK_SUCCESS) {
        qCritical() << "Failed to create Vulkan device:" << result;
        return false;
    }

    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
    return true;
}

bool WaylandVulkanContext::createVulkanSurface(wl_display *display, wl_surface *wlSurface)
{
    VkWaylandSurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.display = display;
    surfaceInfo.surface = wlSurface;

    VkResult result = vkCreateWaylandSurfaceKHR(m_instance, &surfaceInfo, nullptr, &m_surface);
    if (result != VK_SUCCESS) {
        qCritical() << "Failed to create Wayland Vulkan surface:" << result;
        return false;
    }
    return true;
}

bool WaylandVulkanContext::createSwapchain(int width, int height)
{
    m_width = width;
    m_height = height;

    // Find HDR10 format
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());

    m_swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    m_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    m_isHdr = false;

    for (const auto &fmt : formats) {
        if (fmt.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            m_swapchainFormat = fmt.format;
            m_colorSpace = fmt.colorSpace;
            m_isHdr = true;
            qInfo() << "Using HDR10 format:" << fmt.format;
            break;
        }
    }

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps);

    VkSwapchainCreateInfoKHR swapchainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchainInfo.surface = m_surface;
    swapchainInfo.minImageCount = caps.minImageCount + 1;
    swapchainInfo.imageFormat = m_swapchainFormat;
    swapchainInfo.imageColorSpace = m_colorSpace;
    swapchainInfo.imageExtent = {(uint32_t)width, (uint32_t)height};
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.preTransform = caps.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;

    VkResult result = vkCreateSwapchainKHR(m_device, &swapchainInfo, nullptr, &m_swapchain);
    if (result != VK_SUCCESS) {
        qCritical() << "Failed to create swapchain:" << result;
        return false;
    }

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
    m_swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());

    m_swapchainViews.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = m_swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_swapchainFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &viewInfo, nullptr, &m_swapchainViews[i]);
    }

    if (m_isHdr) {
        setHdrMetadata();
    }

    qInfo() << "Swapchain created:" << width << "x" << height << "HDR=" << m_isHdr;
    return true;
}

bool WaylandVulkanContext::recreateSwapchain(int width, int height)
{
    vkDeviceWaitIdle(m_device);
    destroySwapchain();

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps);

    m_width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, (uint32_t)width));
    m_height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, (uint32_t)height));

    return createSwapchain(m_width, m_height);
}

void WaylandVulkanContext::destroySwapchain()
{
    for (auto view : m_swapchainViews) {
        vkDestroyImageView(m_device, view, nullptr);
    }
    m_swapchainViews.clear();
    m_swapchainImages.clear();

    if (m_swapchain) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

void WaylandVulkanContext::setHdrMetadata()
{
    auto vkSetHdrMetadataEXT = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(m_device, "vkSetHdrMetadataEXT");
    if (!vkSetHdrMetadataEXT)
        return;

    VkHdrMetadataEXT hdrMetadata{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
    // BT.2020 primaries
    hdrMetadata.displayPrimaryRed = {0.708f, 0.292f};
    hdrMetadata.displayPrimaryGreen = {0.170f, 0.797f};
    hdrMetadata.displayPrimaryBlue = {0.131f, 0.046f};
    hdrMetadata.whitePoint = {0.3127f, 0.3290f};  // D65
    hdrMetadata.maxLuminance = 1000.0f;
    hdrMetadata.minLuminance = 0.001f;
    hdrMetadata.maxContentLightLevel = 1000.0f;
    hdrMetadata.maxFrameAverageLightLevel = 200.0f;

    vkSetHdrMetadataEXT(m_device, 1, &m_swapchain, &hdrMetadata);
    qInfo() << "HDR metadata set";
}

void WaylandVulkanContext::cleanup()
{
    if (m_device) {
        vkDeviceWaitIdle(m_device);
        destroySwapchain();
    }

    if (m_surface) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }

    if (m_device) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    if (m_instance) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

#endif // USE_WAYLAND_SUBSURFACE
