#ifdef _WIN32

#include "platform/windows_video_surface.h"
#include "logging.h"
#include <SDL3/SDL.h>
#include <d3d11_4.h>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

// Required device extensions for mpv/libplacebo on Windows.
// VK_KHR_swapchain is not used directly (presentation goes through DXGI),
// but libplacebo requires it during pl_vulkan_import.
static const char* s_requiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
};

WindowsVideoSurface::WindowsVideoSurface() = default;

WindowsVideoSurface::~WindowsVideoSurface() {
    cleanup();
}

bool WindowsVideoSurface::initD3D11(SDL_Window* window) {
    parent_window_ = window;

    // Get HWND from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    parent_hwnd_ = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!parent_hwnd_) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] Failed to get HWND from SDL");
        return false;
    }

    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
    D3D_FEATURE_LEVEL actualLevel;
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createFlags, &featureLevel, 1,
        D3D11_SDK_VERSION, &d3d_device_, &actualLevel, &d3d_context_);
    if (FAILED(hr)) {
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               createFlags, &featureLevel, 1,
                               D3D11_SDK_VERSION, &d3d_device_, &actualLevel, &d3d_context_);
        if (FAILED(hr)) {
            LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] D3D11CreateDevice failed: 0x%08lx", hr);
            return false;
        }
    }
    LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] D3D11 device created (feature level %d.%d)",
             (actualLevel >> 12) & 0xF, (actualLevel >> 8) & 0xF);

    // Enable D3D11 multithread protection — video render thread and main thread
    // both use the immediate context (submitFrame vs overlay end)
    ID3D11Multithread* mt = nullptr;
    hr = d3d_device_->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt);
    if (SUCCEEDED(hr) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
        LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] D3D11 multithread protection enabled");
    }

    // Get adapter LUID for matching Vulkan physical device
    IDXGIDevice* dxgi_device = nullptr;
    hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] QueryInterface IDXGIDevice failed");
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    dxgi_device->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC adapterDesc;
    adapter->GetDesc(&adapterDesc);
    adapter_luid_ = adapterDesc.AdapterLuid;
    adapter->Release();

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] D3D11 adapter: %ls (LUID: %lx:%lx)",
             adapterDesc.Description, adapter_luid_.HighPart, adapter_luid_.LowPart);

    // Create DirectComposition device
    hr = DCompositionCreateDevice(dxgi_device, __uuidof(IDCompositionDevice), (void**)&dcomp_device_);
    dxgi_device->Release();
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] DCompositionCreateDevice failed: 0x%08lx", hr);
        return false;
    }

    // Create composition target (topmost=TRUE -> above window content).
    // Both video and CEF overlay visuals are DComp children with explicit
    // Z-ordering, so we don't rely on DWM per-pixel alpha from WGL.
    hr = dcomp_device_->CreateTargetForHwnd(parent_hwnd_, TRUE, &dcomp_target_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] CreateTargetForHwnd failed: 0x%08lx", hr);
        return false;
    }

    // Create visual tree
    hr = dcomp_device_->CreateVisual(&root_visual_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] CreateVisual (root) failed");
        return false;
    }
    dcomp_target_->SetRoot(root_visual_);

    hr = dcomp_device_->CreateVisual(&video_visual_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] CreateVisual (video) failed");
        return false;
    }

    // Always add video visual to root (overlay visual will be a child of video_visual,
    // and DComp children render ON TOP of their parent's content — guaranteeing overlay
    // is always above video regardless of z-order)
    root_visual_->AddVisual(video_visual_, FALSE, nullptr);
    dcomp_device_->Commit();

    // Enable per-pixel transparency via DWM
    MARGINS margins = {-1, -1, -1, -1};
    hr = DwmExtendFrameIntoClientArea(parent_hwnd_, &margins);
    if (FAILED(hr)) {
        LOG_WARN(LOG_PLATFORM, "[WindowsVideoSurface] DwmExtendFrameIntoClientArea failed: 0x%08lx", hr);
    }

    is_hdr_ = detectHdrCapability();
    if (is_hdr_) {
        dxgi_format_ = DXGI_FORMAT_R10G10B10A2_UNORM;
        vk_format_ = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] HDR mode enabled, using R10G10B10A2");
    }

    dcomp_device_->Commit();
    LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] D3D11 + DComp initialized");
    return true;
}

bool WindowsVideoSurface::detectHdrCapability() {
    IDXGIDevice* dxgi_device = nullptr;
    HRESULT hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    if (FAILED(hr)) return false;

    IDXGIAdapter* adapter = nullptr;
    dxgi_device->GetAdapter(&adapter);
    dxgi_device->Release();

    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(0, &output);
    adapter->Release();
    if (FAILED(hr) || !output) return false;

    IDXGIOutput6* output6 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&output6);
    output->Release();
    if (FAILED(hr) || !output6) return false;

    DXGI_OUTPUT_DESC1 desc1;
    hr = output6->GetDesc1(&desc1);
    output6->Release();
    if (FAILED(hr)) return false;

    bool hdr = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] Display HDR: %s (colorSpace=%d, maxLuminance=%.0f, minLuminance=%.4f)",
             hdr ? "yes" : "no", desc1.ColorSpace, desc1.MaxLuminance, desc1.MinLuminance);
    return hdr;
}

IDXGIFactory2* WindowsVideoSurface::getDxgiFactory() {
    IDXGIDevice* dxgi_device = nullptr;
    HRESULT hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    if (FAILED(hr) || !dxgi_device) return nullptr;

    IDXGIAdapter* adapter = nullptr;
    dxgi_device->GetAdapter(&adapter);
    dxgi_device->Release();
    if (!adapter) return nullptr;

    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    adapter->Release();
    return factory;
}

bool WindowsVideoSurface::initVulkan() {
    // Create Vulkan instance
    const char* instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
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
    instanceInfo.enabledExtensionCount = sizeof(instanceExts) / sizeof(instanceExts[0]);
    instanceInfo.ppEnabledExtensionNames = instanceExts;

    if (vkCreateInstance(&instanceInfo, nullptr, &instance_) != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] Failed to create Vulkan instance");
        return false;
    }

    // Enumerate physical devices and match by LUID
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] No Vulkan devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance_, &gpuCount, gpus.data());

    physical_device_ = VK_NULL_HANDLE;
    for (const auto& gpu : gpus) {
        VkPhysicalDeviceIDProperties idProps{};
        idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &idProps;
        vkGetPhysicalDeviceProperties2(gpu, &props2);

        if (idProps.deviceLUIDValid &&
            memcmp(idProps.deviceLUID, &adapter_luid_, sizeof(LUID)) == 0) {
            physical_device_ = gpu;
            LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] Matched Vulkan device: %s",
                     props2.properties.deviceName);
            break;
        }
    }

    if (physical_device_ == VK_NULL_HANDLE) {
        // Fallback to first GPU
        physical_device_ = gpus[0];
        VkPhysicalDeviceProperties gpuProps;
        vkGetPhysicalDeviceProperties(physical_device_, &gpuProps);
        LOG_WARN(LOG_PLATFORM, "[WindowsVideoSurface] No LUID match, using first GPU: %s",
                 gpuProps.deviceName);
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

    // Build extension list
    enabled_extensions_.clear();
    constexpr int requiredCount = sizeof(s_requiredDeviceExtensions) / sizeof(s_requiredDeviceExtensions[0]);
    for (int i = 0; i < requiredCount; i++) {
        if (!hasExtension(s_requiredDeviceExtensions[i])) {
            LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] Missing required extension: %s",
                      s_requiredDeviceExtensions[i]);
            return false;
        }
        enabled_extensions_.push_back(s_requiredDeviceExtensions[i]);
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

    VkResult result = vkCreateDevice(physical_device_, &deviceInfo, nullptr, &device_);
    if (result != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] Failed to create Vulkan device: %d", result);
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] Vulkan device created");
    return true;
}

bool WindowsVideoSurface::init(SDL_Window* window, VkInstance, VkPhysicalDevice,
                                 VkDevice, uint32_t,
                                 const char* const*, int,
                                 const VkPhysicalDeviceFeatures2*) {
    // We ignore the passed-in Vulkan handles and create our own
    if (!initD3D11(window)) return false;
    if (!initVulkan()) return false;
    return true;
}

bool WindowsVideoSurface::createSharedResources(int width, int height) {
    // Create D3D11 staging texture with NT handle sharing
    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = dxgi_format_;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = d3d_device_->CreateTexture2D(&tex_desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] CreateTexture2D (staging) failed: 0x%08lx", hr);
        return false;
    }

    // Get NT handle for cross-API sharing
    IDXGIResource1* dxgi_resource = nullptr;
    hr = staging_texture_->QueryInterface(__uuidof(IDXGIResource1), (void**)&dxgi_resource);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] QueryInterface IDXGIResource1 failed: 0x%08lx", hr);
        return false;
    }

    hr = dxgi_resource->CreateSharedHandle(
        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr, &shared_handle_);
    dxgi_resource->Release();
    if (FAILED(hr) || !shared_handle_) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] CreateSharedHandle failed: 0x%08lx", hr);
        return false;
    }

    // Create Vulkan image that uses imported D3D11 memory
    VkExternalMemoryImageCreateInfo extMemImageInfo{};
    extMemImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extMemImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &extMemImageInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = vk_format_;
    imageInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult vkResult = vkCreateImage(device_, &imageInfo, nullptr, &shared_image_);
    if (vkResult != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] vkCreateImage failed: %d", vkResult);
        return false;
    }

    // Get memory requirements
    VkMemoryDedicatedRequirements dedicatedReqs{};
    dedicatedReqs.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;

    VkMemoryRequirements2 memReqs2{};
    memReqs2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    memReqs2.pNext = &dedicatedReqs;

    VkImageMemoryRequirementsInfo2 imageReqsInfo{};
    imageReqsInfo.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    imageReqsInfo.image = shared_image_;

    vkGetImageMemoryRequirements2(device_, &imageReqsInfo, &memReqs2);

    // Find device-local memory type
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memProps);

    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReqs2.memoryRequirements.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] No suitable memory type found");
        return false;
    }

    // Import D3D11 memory into Vulkan using dedicated allocation
    VkMemoryDedicatedAllocateInfo dedicatedAllocInfo{};
    dedicatedAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedAllocInfo.image = shared_image_;

    VkImportMemoryWin32HandleInfoKHR importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    importInfo.pNext = &dedicatedAllocInfo;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    importInfo.handle = shared_handle_;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = memReqs2.memoryRequirements.size;
    allocInfo.memoryTypeIndex = memoryType;

    vkResult = vkAllocateMemory(device_, &allocInfo, nullptr, &imported_memory_);
    if (vkResult != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] vkAllocateMemory (import) failed: %d", vkResult);
        return false;
    }

    vkResult = vkBindImageMemory(device_, shared_image_, imported_memory_, 0);
    if (vkResult != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] vkBindImageMemory failed: %d", vkResult);
        return false;
    }

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = shared_image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = vk_format_;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkResult = vkCreateImageView(device_, &viewInfo, nullptr, &shared_view_);
    if (vkResult != VK_SUCCESS) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] vkCreateImageView failed: %d", vkResult);
        return false;
    }

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] Shared resources created: %dx%d", width, height);
    return true;
}

bool WindowsVideoSurface::createSwapchain(int width, int height) {
    destroySwapchain();

    width_ = width;
    height_ = height;

    // Create composition swap chain
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = dxgi_format_;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    IDXGIFactory2* factory = getDxgiFactory();
    if (!factory) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] Failed to get DXGI factory");
        return false;
    }

    IDXGISwapChain1* swap_chain1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForComposition(d3d_device_, &desc, nullptr, &swap_chain1);
    factory->Release();
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] CreateSwapChainForComposition failed: 0x%08lx", hr);
        return false;
    }

    hr = swap_chain1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&swap_chain_);
    swap_chain1->Release();
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] QueryInterface IDXGISwapChain3 failed: 0x%08lx", hr);
        return false;
    }

    if (is_hdr_) {
        hr = swap_chain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        if (FAILED(hr)) {
            LOG_WARN(LOG_PLATFORM, "[WindowsVideoSurface] SetColorSpace1 failed: 0x%08lx, falling back to SDR", hr);
            is_hdr_ = false;
            dxgi_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
            vk_format_ = VK_FORMAT_B8G8R8A8_UNORM;
            // Recreate swap chain with SDR format (CopyResource requires matching formats)
            swap_chain_->Release();
            swap_chain_ = nullptr;
            desc.Format = dxgi_format_;
            factory = getDxgiFactory();
            if (!factory) {
                LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] SDR fallback: failed to get DXGI factory");
                return false;
            }
            IDXGISwapChain1* sc1 = nullptr;
            hr = factory->CreateSwapChainForComposition(d3d_device_, &desc, nullptr, &sc1);
            factory->Release();
            if (FAILED(hr)) {
                LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] SDR swap chain fallback failed: 0x%08lx", hr);
                return false;
            }
            hr = sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&swap_chain_);
            sc1->Release();
            if (FAILED(hr)) {
                LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] SDR fallback: QueryInterface IDXGISwapChain3 failed: 0x%08lx", hr);
                return false;
            }
        }
    }

    // Clear to #101010 (matches window background, avoids white flash before first video frame)
    {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
            ID3D11RenderTargetView* rtv = nullptr;
            if (SUCCEEDED(d3d_device_->CreateRenderTargetView(bb, nullptr, &rtv))) {
                float bg[4] = {0x10 / 255.0f, 0x10 / 255.0f, 0x10 / 255.0f, 1};
                d3d_context_->ClearRenderTargetView(rtv, bg);
                rtv->Release();
            }
            bb->Release();
            swap_chain_->Present(0, 0);
        }
    }

    video_visual_->SetContent(swap_chain_);

    // Create shared Vulkan/D3D11 resources
    if (!createSharedResources(width, height)) {
        return false;
    }

    dcomp_device_->Commit();
    LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] Swap chain created: %dx%d (HDR: %s)",
             width, height, is_hdr_ ? "yes" : "no");
    return true;
}

bool WindowsVideoSurface::startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat) {
    if (!shared_image_) return false;

    frame_active_ = true;
    *outImage = shared_image_;
    *outView = shared_view_;
    *outFormat = vk_format_;
    return true;
}

void WindowsVideoSurface::submitFrame() {
    if (!frame_active_ || !swap_chain_ || !staging_texture_) return;

    // No explicit Vulkan sync needed here: mpv's done_frame callback calls
    // pl_gpu_finish() which already drains this queue before returning.

    // Lock D3D11/DComp — main thread's overlay end() uses the same context
    std::lock_guard<std::mutex> lock(d3d_mutex_);

    // Copy from shared staging texture to swap chain back buffer
    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back_buffer);
    if (FAILED(hr)) {
        LOG_ERROR(LOG_PLATFORM, "[WindowsVideoSurface] GetBuffer failed: 0x%08lx", hr);
        frame_active_ = false;
        return;
    }

    d3d_context_->CopyResource(back_buffer, staging_texture_);
    back_buffer->Release();

    swap_chain_->Present(0, 0);
    dcomp_device_->Commit();

    frame_active_ = false;
}

bool WindowsVideoSurface::recreateSwapchain(int width, int height) {
    if (static_cast<int>(width_) == width && static_cast<int>(height_) == height) return true;

    LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] Recreating swap chain: %dx%d -> %dx%d",
             width_, height_, width, height);

    if (device_) {
        vkQueueWaitIdle(queue_);
    }

    destroySharedResources();
    destroySwapchain();
    return createSwapchain(width, height);
}

void WindowsVideoSurface::show() {
    if (!visible_ && video_visual_ && swap_chain_) {
        video_visual_->SetContent(swap_chain_);
        dcomp_device_->Commit();
        visible_ = true;
        LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] Video visual shown");
    }
}

void WindowsVideoSurface::hide() {
    if (visible_ && video_visual_) {
        video_visual_->SetContent(nullptr);
        dcomp_device_->Commit();
        visible_ = false;
        LOG_INFO(LOG_PLATFORM, "[WindowsVideoSurface] Video visual hidden");
    }
}

void WindowsVideoSurface::destroySharedResources() {
    if (shared_view_) {
        vkDestroyImageView(device_, shared_view_, nullptr);
        shared_view_ = VK_NULL_HANDLE;
    }
    if (shared_image_) {
        vkDestroyImage(device_, shared_image_, nullptr);
        shared_image_ = VK_NULL_HANDLE;
    }
    if (imported_memory_) {
        vkFreeMemory(device_, imported_memory_, nullptr);
        imported_memory_ = VK_NULL_HANDLE;
    }
    if (shared_handle_) {
        CloseHandle(shared_handle_);
        shared_handle_ = nullptr;
    }
    if (staging_texture_) {
        staging_texture_->Release();
        staging_texture_ = nullptr;
    }
}

void WindowsVideoSurface::destroySwapchain() {
    destroySharedResources();
    if (swap_chain_) {
        swap_chain_->Release();
        swap_chain_ = nullptr;
    }
}

void WindowsVideoSurface::cleanup() {
    if (device_) {
        vkQueueWaitIdle(queue_);
    }

    destroySwapchain();

    // Vulkan cleanup
    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    // DComp cleanup
    if (video_visual_) {
        video_visual_->Release();
        video_visual_ = nullptr;
    }
    if (root_visual_) {
        root_visual_->Release();
        root_visual_ = nullptr;
    }
    if (dcomp_target_) {
        dcomp_target_->Release();
        dcomp_target_ = nullptr;
    }
    if (dcomp_device_) {
        dcomp_device_->Release();
        dcomp_device_ = nullptr;
    }

    // D3D11 cleanup
    if (d3d_context_) {
        d3d_context_->Release();
        d3d_context_ = nullptr;
    }
    if (d3d_device_) {
        d3d_device_->Release();
        d3d_device_ = nullptr;
    }

    visible_ = false;
    frame_active_ = false;
}

#endif // _WIN32
