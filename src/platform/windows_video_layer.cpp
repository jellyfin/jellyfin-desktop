#ifdef _WIN32

#include "platform/windows_video_layer.h"
#include "platform/windows_dxgi_util.h"
#include "logging.h"
#include <SDL3/SDL.h>
#include <cstring>

// ITU-R BT.2408 SDR reference white (cd/m²).
// Same value as PL_COLOR_SDR_WHITE / MP_REF_WHITE.
static constexpr float kSdrWhite = 203.0f;

// Device extensions needed for mpv/libplacebo
static const char* s_requiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
};

// Window class for the child HWND
static const wchar_t* kVideoWindowClass = L"JellyfinVideoChild";

static LRESULT CALLBACK videoChildWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Pass all input through to the parent HWND (where SDL routes it to CEF).
    // The child HWND is for Vulkan presentation only — DComp renders CEF on top.
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool ensureWindowClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = videoChildWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kVideoWindowClass;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] RegisterClassEx failed: %lu", GetLastError());
        return false;
    }
    return true;
}

bool WindowsVideoLayer::init(SDL_Window* window) {
    parent_window_ = window;

    // Get HWND from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    parent_hwnd_ = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!parent_hwnd_) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to get HWND from SDL");
        return false;
    }

    // Create child HWND for video
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(parent_hwnd_, GWLP_HINSTANCE);
    if (!ensureWindowClass(hInstance)) return false;

    RECT rc;
    GetClientRect(parent_hwnd_, &rc);

    child_hwnd_ = CreateWindowExW(
        0, kVideoWindowClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        parent_hwnd_, nullptr, hInstance, nullptr);

    if (!child_hwnd_) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] CreateWindowEx (child) failed: %lu", GetLastError());
        return false;
    }

    if (!initVulkan()) return false;

    queryDisplayProfile();

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Initialized (child HWND + Vulkan, HDR: %s)",
             is_hdr_ ? "yes" : "no");
    return true;
}

bool WindowsVideoLayer::initVulkan() {
    const char* instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_2;
    appInfo.pApplicationName = "Jellyfin Desktop CEF";

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = sizeof(instanceExts) / sizeof(instanceExts[0]);
    instanceInfo.ppEnabledExtensionNames = instanceExts;

    if (vkCreateInstance(&instanceInfo, nullptr, &instance_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to create Vulkan instance");
        return false;
    }

    // Enumerate physical devices and match by LUID (same GPU as DXGI adapter)
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] No Vulkan devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance_, &gpuCount, gpus.data());

    // Select the first Vulkan device with a valid LUID (for DXGI adapter matching)
    physical_device_ = gpus[0];
    for (const auto& gpu : gpus) {
        VkPhysicalDeviceIDProperties idProps{};
        idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &idProps;
        vkGetPhysicalDeviceProperties2(gpu, &props2);

        if (idProps.deviceLUIDValid) {
            // Store LUID from the first valid device for WindowsDCompContext matching
            if (adapter_luid_.LowPart == 0 && adapter_luid_.HighPart == 0)
                memcpy(&adapter_luid_, idProps.deviceLUID, sizeof(LUID));

            physical_device_ = gpu;
            LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Using Vulkan device: %s",
                     props2.properties.deviceName);
            break;
        }
    }

    // Get available device extensions
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
    for (const auto& name : s_requiredDeviceExtensions) {
        if (!hasExtension(name)) {
            LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Missing required extension: %s", name);
            return false;
        }
        enabled_extensions_.push_back(name);
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

    // Create device with features for mpv/libplacebo
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

    if (vkCreateDevice(physical_device_, &deviceInfo, nullptr, &device_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to create Vulkan device");
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    // Create VkSurface from child HWND
    VkWin32SurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = (HINSTANCE)GetWindowLongPtrW(child_hwnd_, GWLP_HINSTANCE);
    surfaceInfo.hwnd = child_hwnd_;

    if (vkCreateWin32SurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoLayer] Failed to create Vulkan Win32 surface");
        return false;
    }

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Vulkan initialized (libplacebo swapchain mode)");
    return true;
}

void WindowsVideoLayer::queryDisplayProfile() {
    // Query DXGI output for display HDR capabilities — same approach as
    // standalone mpv's context_win.c:preferred_csp().
    IDXGIAdapter1* adapter = findDxgiAdapterByLuid(adapter_luid_);
    if (!adapter) return;

    IDXGIOutput* output = nullptr;
    if (FAILED(adapter->EnumOutputs(0, &output))) { adapter->Release(); return; }
    adapter->Release();

    IDXGIOutput6* output6 = nullptr;
    if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&output6))) { output->Release(); return; }
    output->Release();

    DXGI_OUTPUT_DESC1 desc;
    HRESULT hr = output6->GetDesc1(&desc);
    output6->Release();
    if (FAILED(hr)) return;

    is_hdr_ = (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);

    if (desc.MaxLuminance > 0) {
        display_profile_.max_luma = desc.MaxLuminance;
        display_profile_.min_luma = desc.MinLuminance;
        // ref_luma = PL_COLOR_SDR_WHITE makes the scaling in context.c an identity,
        // since DXGI reports luminance in absolute nits already.
        display_profile_.ref_luma = kSdrWhite;
    }

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoLayer] Display HDR: %s (max=%.0f min=%.4f nits)",
             is_hdr_ ? "yes" : "no", desc.MaxLuminance, desc.MinLuminance);
}

bool WindowsVideoLayer::createSwapchain(int width, int height) {
    // No-op: libplacebo manages the swapchain via the VkSurface.
    resize(width, height);
    return true;
}

void WindowsVideoLayer::resize(uint32_t width, uint32_t height) {
    if (width_ == width && height_ == height) return;
    width_ = width;
    height_ = height;

    if (child_hwnd_) {
        SetWindowPos(child_hwnd_, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void WindowsVideoLayer::setVisible(bool visible) {
    if (child_hwnd_) {
        ShowWindow(child_hwnd_, visible ? SW_SHOWNA : SW_HIDE);
    }
}

void WindowsVideoLayer::cleanup() {
    if (device_) {
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

    if (child_hwnd_) {
        DestroyWindow(child_hwnd_);
        child_hwnd_ = nullptr;
    }
}

#endif // _WIN32
