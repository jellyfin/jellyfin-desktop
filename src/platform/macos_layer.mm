#ifdef __APPLE__

#import "macos_layer.h"
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include <vector>
#include <algorithm>

// Vulkan surface extension for macOS
#include <vulkan/vulkan_metal.h>

// Device extensions needed for mpv/libplacebo (MoltenVK compatible)
static const char* s_deviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    "VK_KHR_portability_subset",  // Required for MoltenVK
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    VK_KHR_MAINTENANCE2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE3_EXTENSION_NAME,
    VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME,
    VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    VK_KHR_SPIRV_1_4_EXTENSION_NAME,
    VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
};
static const int s_deviceExtensionCount = sizeof(s_deviceExtensions) / sizeof(s_deviceExtensions[0]);

bool MacOSVideoLayer::init(SDL_Window* window, VkInstance, VkPhysicalDevice,
                           VkDevice, uint32_t,
                           const char* const*, uint32_t,
                           const char* const*) {
    // We ignore the passed-in Vulkan handles and create our own
    // This matches how WaylandSubsurface works on Linux
    window_ = window;

    // Get NSWindow from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    NSWindow* ns_window = (__bridge NSWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!ns_window) {
        NSLog(@"Failed to get NSWindow from SDL");
        return false;
    }

    // Create a subview for video (behind the main content view)
    NSView* content_view = [ns_window contentView];
    NSRect frame = [content_view bounds];

    video_view_ = [[NSView alloc] initWithFrame:frame];
    [video_view_ setWantsLayer:YES];
    [video_view_ setLayerContentsRedrawPolicy:NSViewLayerContentsRedrawDuringViewResize];
    [video_view_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    // Create CAMetalLayer with HDR support
    metal_layer_ = [CAMetalLayer layer];
    metal_layer_.device = MTLCreateSystemDefaultDevice();
    metal_layer_.pixelFormat = MTLPixelFormatRGBA16Float;  // HDR format
    metal_layer_.wantsExtendedDynamicRangeContent = YES;
    CGColorSpaceRef colorspace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    metal_layer_.colorspace = colorspace;
    CGColorSpaceRelease(colorspace);
    metal_layer_.framebufferOnly = YES;
    metal_layer_.frame = frame;

    // Disable implicit animations to prevent jelly effect during resize
    // Note: presentsWithTransaction doesn't work well with Vulkan/MoltenVK
    metal_layer_.actions = @{
        @"bounds": [NSNull null],
        @"position": [NSNull null],
        @"contents": [NSNull null],
        @"anchorPoint": [NSNull null]
    };
    metal_layer_.contentsGravity = kCAGravityTopLeft;
    metal_layer_.anchorPoint = CGPointMake(0, 0);

    [video_view_ setLayer:metal_layer_];

    // Add video view as first subview (at back)
    // The MetalCompositor will add CEF layer on top
    [content_view addSubview:video_view_ positioned:NSWindowBelow relativeTo:nil];

    is_hdr_ = true;
    NSLog(@"MacOS video layer initialized with HDR (EDR) support");

    // Create our own Vulkan instance (like WaylandSubsurface does)
    const char* instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_METAL_SURFACE_EXTENSION_NAME,
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_2;
    appInfo.pApplicationName = "Jellyfin Desktop";

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 4;
    instanceInfo.ppEnabledExtensionNames = instanceExts;
    instanceInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

    if (vkCreateInstance(&instanceInfo, nullptr, &instance_) != VK_SUCCESS) {
        NSLog(@"Failed to create Vulkan instance");
        return false;
    }

    // Select physical device
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr);
    if (gpuCount == 0) {
        NSLog(@"No Vulkan devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance_, &gpuCount, gpus.data());
    physical_device_ = gpus[0];

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

    // Create device
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queue_family_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    // Query supported features first (feature chain for libplacebo/mpv)
    ycbcr_features_ = {};
    ycbcr_features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;

    host_query_reset_features_ = {};
    host_query_reset_features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
    host_query_reset_features_.pNext = &ycbcr_features_;

    timeline_features_ = {};
    timeline_features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline_features_.pNext = &host_query_reset_features_;

    features2_ = {};
    features2_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2_.pNext = &timeline_features_;

    // Query what features are actually supported
    vkGetPhysicalDeviceFeatures2(physical_device_, &features2_);

    NSLog(@"Vulkan features - shaderImageGatherExtended: %d, shaderStorageImageReadWithoutFormat: %d",
          features2_.features.shaderImageGatherExtended,
          features2_.features.shaderStorageImageReadWithoutFormat);
    NSLog(@"Vulkan features - timelineSemaphore: %d, samplerYcbcrConversion: %d, hostQueryReset: %d",
          timeline_features_.timelineSemaphore,
          ycbcr_features_.samplerYcbcrConversion,
          host_query_reset_features_.hostQueryReset);

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &features2_;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = s_deviceExtensionCount;
    deviceInfo.ppEnabledExtensionNames = s_deviceExtensions;

    if (vkCreateDevice(physical_device_, &deviceInfo, nullptr, &device_) != VK_SUCCESS) {
        NSLog(@"Failed to create Vulkan device");
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    // Store device extensions for mpv
    device_extensions_ = s_deviceExtensions;
    device_extension_count_ = s_deviceExtensionCount;

    // Create Vulkan surface from Metal layer
    VkMetalSurfaceCreateInfoEXT surfaceCreateInfo = {};
    surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    surfaceCreateInfo.pLayer = metal_layer_;

    PFN_vkCreateMetalSurfaceEXT vkCreateMetalSurfaceEXT =
        (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(instance_, "vkCreateMetalSurfaceEXT");

    if (!vkCreateMetalSurfaceEXT) {
        NSLog(@"vkCreateMetalSurfaceEXT not available");
        return false;
    }

    VkResult result = vkCreateMetalSurfaceEXT(instance_, &surfaceCreateInfo, nullptr, &surface_);
    if (result != VK_SUCCESS) {
        NSLog(@"Failed to create Vulkan Metal surface: %d", result);
        return false;
    }

    NSLog(@"Vulkan context initialized (manual instance/device via MoltenVK)");
    return true;
}

void MacOSVideoLayer::cleanup() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    destroySwapchain();

    if (image_available_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, image_available_, nullptr);
        image_available_ = VK_NULL_HANDLE;
    }
    if (render_finished_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, render_finished_, nullptr);
        render_finished_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    if (video_view_) {
        [video_view_ removeFromSuperview];
        video_view_ = nil;
    }
    metal_layer_ = nil;
}

bool MacOSVideoLayer::createSwapchain(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;

    // Update Metal layer size
    metal_layer_.drawableSize = CGSizeMake(width, height);

    // Destroy old image views (swapchain is passed as oldSwapchain, Vulkan handles retirement)
    for (uint32_t i = 0; i < image_count_; i++) {
        if (image_views_[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, image_views_[i], nullptr);
            image_views_[i] = VK_NULL_HANDLE;
        }
    }

    // Query surface capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);

    // Choose HDR format if available
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, formats.data());

    // Prefer HDR formats
    format_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    color_space_ = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;

    // Check if our preferred format is supported
    bool found = false;
    for (const auto& fmt : formats) {
        if (fmt.format == format_ && fmt.colorSpace == color_space_) {
            found = true;
            break;
        }
    }

    if (!found && !formats.empty()) {
        // Fall back to first available
        format_ = formats[0].format;
        color_space_ = formats[0].colorSpace;
        is_hdr_ = false;
        NSLog(@"HDR format not available, falling back to SDR");
    }

    // Create swapchain
    VkSwapchainKHR oldSwapchain = swapchain_;

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = std::max(2u, capabilities.minImageCount);
    createInfo.imageFormat = format_;
    createInfo.imageColorSpace = color_space_;
    createInfo.imageExtent = {width, height};
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    VkResult result = vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_);

    // Destroy old swapchain after new one is created (Vulkan retired it, but we must still destroy)
    if (oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
    }

    if (result != VK_SUCCESS) {
        NSLog(@"Failed to create swapchain: %d", result);
        return false;
    }

    // Get swapchain images
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, nullptr);
    image_count_ = std::min(image_count_, MAX_IMAGES);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, images_);

    // Create image views
    for (uint32_t i = 0; i < image_count_; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &viewInfo, nullptr, &image_views_[i]) != VK_SUCCESS) {
            NSLog(@"Failed to create image view %d", i);
            return false;
        }
    }

    // Create semaphores for frame sync
    if (image_available_ == VK_NULL_HANDLE) {
        VkSemaphoreCreateInfo semInfo = {};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &semInfo, nullptr, &image_available_);
        vkCreateSemaphore(device_, &semInfo, nullptr, &render_finished_);
    }

    NSLog(@"Swapchain created: %dx%d format=%d colorSpace=%d HDR=%s",
          width, height, format_, color_space_, is_hdr_ ? "yes" : "no");

    return true;
}

void MacOSVideoLayer::destroySwapchain() {
    for (uint32_t i = 0; i < image_count_; i++) {
        if (image_views_[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, image_views_[i], nullptr);
            image_views_[i] = VK_NULL_HANDLE;
        }
    }

    if (swapchain_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    image_count_ = 0;
}

bool MacOSVideoLayer::startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat) {
    if (frame_active_) {
        return false;
    }

    // Recreate swapchain if size changed
    if (needs_swapchain_recreate_ || swapchain_ == VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        createSwapchain(width_, height_);
        needs_swapchain_recreate_ = false;
    }

    if (swapchain_ == VK_NULL_HANDLE) {
        return false;
    }

    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                             image_available_, VK_NULL_HANDLE,
                                             &current_image_idx_);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        needs_swapchain_recreate_ = true;
        return false;
    }
    // VK_SUBOPTIMAL_KHR means image is usable - continue (MoltenVK often returns this)
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return false;
    }

    frame_active_ = true;
    *outImage = images_[current_image_idx_];
    *outView = image_views_[current_image_idx_];
    *outFormat = format_;
    return true;
}

void MacOSVideoLayer::submitFrame() {
    if (!frame_active_) {
        return;
    }

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 0;  // mpv handles its own sync
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &current_image_idx_;

    vkQueuePresentKHR(queue_, &presentInfo);
    frame_active_ = false;
}

void MacOSVideoLayer::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) {
        return;
    }

    width_ = width;
    height_ = height;
    needs_swapchain_recreate_ = true;
}

void MacOSVideoLayer::setVisible(bool visible) {
    if (video_view_) {
        [video_view_ setHidden:!visible];
    }
}

void MacOSVideoLayer::setPosition(int x, int y) {
    if (video_view_) {
        NSRect frame = [video_view_ frame];
        frame.origin.x = x;
        frame.origin.y = y;
        [video_view_ setFrame:frame];
    }
}

#endif // __APPLE__
