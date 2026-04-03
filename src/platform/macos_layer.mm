#ifdef __APPLE__

#import "macos_layer.h"
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include <vector>
#include <libplacebo/colorspace.h>

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

    // Create CAMetalLayer for video presentation.
    // RGBA16Float so MoltenVK offers HDR-capable formats.
    // wantsExtendedDynamicRangeContent allows values > 1.0 to reach the
    // display — without it macOS clips HDR highlights. The per-frame
    // colorspace hint in libmpv_gpu_next.c switches the swapchain between
    // sRGB (SDR) and extended linear sRGB (HDR) based on the video content.
    metal_layer_ = [CAMetalLayer layer];
    metal_layer_.device = MTLCreateSystemDefaultDevice();
    metal_layer_.pixelFormat = MTLPixelFormatRGBA16Float;
    metal_layer_.wantsExtendedDynamicRangeContent = YES;
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

    NSLog(@"MacOS video layer initialized");

    // Create our own Vulkan instance (like WaylandSubsurface does)
    const char* instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_METAL_SURFACE_EXTENSION_NAME,
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_2;
    appInfo.pApplicationName = "Jellyfin Desktop";

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 5;
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

    queryDisplayProfile();

    NSLog(@"Vulkan context initialized (libplacebo swapchain mode via MoltenVK)");
    return true;
}

void MacOSVideoLayer::cleanup() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
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
    // No-op: libplacebo manages the swapchain via the VkSurface.
    resize(width, height);
    return true;
}

void MacOSVideoLayer::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) {
        return;
    }

    width_ = width;
    height_ = height;
    metal_layer_.drawableSize = CGSizeMake(width, height);
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

void MacOSVideoLayer::queryDisplayProfile() {
    NSScreen* screen = [NSScreen mainScreen];
    if (!screen) return;

    CGFloat edr_ratio = [screen maximumExtendedDynamicRangeColorComponentValue];
    if (edr_ratio <= 1.0) return;

    is_hdr_ = true;

    // Don't populate display_profile_ — standalone mpv on macOS doesn't pass
    // display luminance info either (no preferred_csp callback in context_mac.m).
    // libplacebo detects EDR capability from the swapchain directly.
    // Passing peak luminance would make tone mapping less aggressive than mpv,
    // causing HDR content to appear brighter.

    NSLog(@"Display EDR: ratio=%.1f (no display profile — libplacebo detects from swapchain)",
          edr_ratio);
}

#endif // __APPLE__
