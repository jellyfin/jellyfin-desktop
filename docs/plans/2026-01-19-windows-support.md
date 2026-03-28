# Windows Support Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add Windows support with mpv rendering on a separate layer (child window) below the CEF overlay, matching the architecture of Linux (Wayland subsurface) and macOS (CAMetalLayer).

**Architecture:** Create a child HWND for video rendering positioned below the main window, with DirectComposition for proper layering. The CEF overlay renders to the main window via WGL/OpenGL, compositing over the video child window. This follows the same pattern as Wayland subsurfaces and macOS NSView layers.

**Tech Stack:** Win32 API (CreateWindowEx, child HWND), Vulkan (VK_KHR_win32_surface), WGL/OpenGL for CEF compositor, DirectComposition for HDR metadata (optional).

---

## Architecture Overview

```
+-----------------------------------------+
|           Main SDL Window (HWND)        |
|  +-----------------------------------+  |
|  |  CEF Overlay (WGL OpenGL - SDR)   |  |  <- Parent window, renders CEF
|  |  - OpenGL compositor              |  |
|  |  - Software or DX interop         |  |
|  +-----------------------------------+  |
|  +-----------------------------------+  |
|  |  Video Child (Vulkan - HDR)       |  |  <- Child HWND, below parent
|  |  - Vulkan swapchain               |  |
|  |  - mpv/libplacebo renders here    |  |
|  |  - HDR via DXGI swapchain hint    |  |
|  +-----------------------------------+  |
+-----------------------------------------+
```

## Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Video window | Child HWND | Matches Wayland subsurface pattern |
| Graphics API | Vulkan | Reuses existing mpv rendering code |
| Window layering | WS_CHILD + SetWindowPos | Simple, no DirectComposition required |
| CEF compositor | WGL OpenGL | Matches Linux path, software rendering |
| HDR support | DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 | Windows 10+ HDR |

## File Structure

```
src/
+-- main.cpp                    # Add #ifdef _WIN32 for Windows setup
+-- platform/
|   +-- wayland_subsurface.cpp/h  # Linux (existing)
|   +-- macos_layer.mm/h          # macOS (existing)
|   +-- windows_video_layer.cpp/h # Windows (NEW)
+-- context/
|   +-- egl_context.cpp/h         # Linux (existing)
|   +-- cgl_context.mm/h          # macOS (existing)
|   +-- wgl_context.cpp/h         # Windows (NEW)
+-- compositor/
|   +-- opengl_compositor.cpp/h   # Shared (add WGL path)
```

## Dependencies

- Windows SDK (Win32 API)
- Vulkan SDK (already required)
- WGL for OpenGL (Windows native)
- CEF Windows build

---

## Task 1: Create WGL Context for CEF Compositor

**Files:**
- Create: `src/context/wgl_context.h`
- Create: `src/context/wgl_context.cpp`

**Step 1: Create WGL context header**

Create `src/context/wgl_context.h`:

```cpp
#pragma once
#ifdef _WIN32

#include <windows.h>
#include <GL/gl.h>
#include <GL/wglext.h>

struct SDL_Window;

class WGLContext {
public:
    WGLContext();
    ~WGLContext();

    bool init(SDL_Window* window);
    void cleanup();
    void makeCurrent();
    void swapBuffers();
    void resize(int width, int height);

    HDC hdc() const { return hdc_; }
    HGLRC hglrc() const { return hglrc_; }

private:
    SDL_Window* window_ = nullptr;
    HWND hwnd_ = nullptr;
    HDC hdc_ = nullptr;
    HGLRC hglrc_ = nullptr;
};

#endif // _WIN32
```

**Step 2: Create WGL context implementation**

Create `src/context/wgl_context.cpp`:

```cpp
#ifdef _WIN32

#include "context/wgl_context.h"
#include <SDL3/SDL.h>
#include <iostream>

WGLContext::WGLContext() = default;

WGLContext::~WGLContext() {
    cleanup();
}

bool WGLContext::init(SDL_Window* window) {
    window_ = window;

    // Get HWND from SDL3
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    hwnd_ = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd_) {
        std::cerr << "Failed to get HWND from SDL window" << std::endl;
        return false;
    }

    hdc_ = GetDC(hwnd_);
    if (!hdc_) {
        std::cerr << "Failed to get DC" << std::endl;
        return false;
    }

    // Set pixel format
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 0;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pixelFormat = ChoosePixelFormat(hdc_, &pfd);
    if (!pixelFormat || !SetPixelFormat(hdc_, pixelFormat, &pfd)) {
        std::cerr << "Failed to set pixel format" << std::endl;
        return false;
    }

    // Create OpenGL context
    hglrc_ = wglCreateContext(hdc_);
    if (!hglrc_) {
        std::cerr << "Failed to create WGL context" << std::endl;
        return false;
    }

    makeCurrent();
    std::cerr << "WGL context initialized" << std::endl;
    return true;
}

void WGLContext::cleanup() {
    if (hglrc_) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hglrc_);
        hglrc_ = nullptr;
    }
    if (hdc_ && hwnd_) {
        ReleaseDC(hwnd_, hdc_);
        hdc_ = nullptr;
    }
}

void WGLContext::makeCurrent() {
    if (hdc_ && hglrc_) {
        wglMakeCurrent(hdc_, hglrc_);
    }
}

void WGLContext::swapBuffers() {
    if (hdc_) {
        SwapBuffers(hdc_);
    }
}

void WGLContext::resize(int width, int height) {
    // WGL doesn't need explicit resize handling
    (void)width;
    (void)height;
}

#endif // _WIN32
```

**Step 3: Commit**

```bash
git add src/context/wgl_context.h src/context/wgl_context.cpp
git commit -m "feat(windows): add WGL context for CEF compositor"
```

---

## Task 2: Create Windows Video Layer

**Files:**
- Create: `src/platform/windows_video_layer.h`
- Create: `src/platform/windows_video_layer.cpp`

**Step 1: Create Windows video layer header**

Create `src/platform/windows_video_layer.h`:

```cpp
#pragma once
#ifdef _WIN32

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <vector>

struct SDL_Window;

class WindowsVideoLayer {
public:
    WindowsVideoLayer();
    ~WindowsVideoLayer();

    bool init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physicalDevice,
              VkDevice device, uint32_t queueFamily,
              const char* const* deviceExtensions, uint32_t deviceExtensionCount,
              const char* const* instanceExtensions);
    void cleanup();

    bool createSwapchain(uint32_t width, uint32_t height);
    void destroySwapchain();

    // Frame acquisition (matches WaylandSubsurface interface)
    bool startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat);
    void submitFrame();

    // Accessors
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    VkDevice vkDevice() const { return device_; }
    VkInstance vkInstance() const { return instance_; }
    VkPhysicalDevice vkPhysicalDevice() const { return physical_device_; }
    VkQueue vkQueue() const { return queue_; }
    uint32_t vkQueueFamily() const { return queue_family_; }
    PFN_vkGetInstanceProcAddr vkGetProcAddr() const { return vkGetInstanceProcAddr; }
    const VkPhysicalDeviceFeatures2* features() const { return &features2_; }
    const char* const* deviceExtensions() const { return device_extensions_; }
    int deviceExtensionCount() const { return device_extension_count_; }

    void resize(uint32_t width, uint32_t height);
    void setVisible(bool visible);
    void setPosition(int x, int y);

    bool isHdr() const { return is_hdr_; }

private:
    SDL_Window* parent_window_ = nullptr;
    HWND parent_hwnd_ = nullptr;
    HWND video_hwnd_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    static constexpr uint32_t MAX_IMAGES = 4;
    VkImage images_[MAX_IMAGES] = {};
    VkImageView image_views_[MAX_IMAGES] = {};
    uint32_t image_count_ = 0;
    uint32_t current_image_idx_ = 0;
    bool frame_active_ = false;

    VkSemaphore image_available_ = VK_NULL_HANDLE;
    VkFence acquire_fence_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool is_hdr_ = false;
    bool visible_ = false;

    // Features/extensions for mpv
    VkPhysicalDeviceFeatures2 features2_{};
    VkPhysicalDeviceVulkan11Features vk11_features_{};
    VkPhysicalDeviceVulkan12Features vk12_features_{};
    const char* const* device_extensions_ = nullptr;
    int device_extension_count_ = 0;

    static LRESULT CALLBACK VideoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

#endif // _WIN32
```

**Step 2: Create Windows video layer implementation**

Create `src/platform/windows_video_layer.cpp`:

```cpp
#ifdef _WIN32

#include "platform/windows_video_layer.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <algorithm>
#include <cstring>

// Device extensions needed for mpv/libplacebo
static const char* s_deviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    VK_KHR_MAINTENANCE2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE3_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
};
static const int s_deviceExtensionCount = sizeof(s_deviceExtensions) / sizeof(s_deviceExtensions[0]);

static const wchar_t* VIDEO_WINDOW_CLASS = L"JellyfinVideoLayer";
static bool s_classRegistered = false;

LRESULT CALLBACK WindowsVideoLayer::VideoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // Prevent flicker
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

WindowsVideoLayer::WindowsVideoLayer() = default;

WindowsVideoLayer::~WindowsVideoLayer() {
    cleanup();
}

bool WindowsVideoLayer::init(SDL_Window* window, VkInstance, VkPhysicalDevice,
                              VkDevice, uint32_t,
                              const char* const*, uint32_t,
                              const char* const*) {
    parent_window_ = window;

    // Get parent HWND from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    parent_hwnd_ = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!parent_hwnd_) {
        std::cerr << "Failed to get parent HWND from SDL" << std::endl;
        return false;
    }

    // Register window class for video child
    if (!s_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = VideoWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = VIDEO_WINDOW_CLASS;
        if (!RegisterClassExW(&wc)) {
            std::cerr << "Failed to register video window class" << std::endl;
            return false;
        }
        s_classRegistered = true;
    }

    // Get parent window size
    RECT parentRect;
    GetClientRect(parent_hwnd_, &parentRect);
    int w = parentRect.right - parentRect.left;
    int h = parentRect.bottom - parentRect.top;

    // Create child window for video (positioned below parent's content)
    video_hwnd_ = CreateWindowExW(
        0,                          // No extended styles
        VIDEO_WINDOW_CLASS,
        L"Video",
        WS_CHILD | WS_VISIBLE,      // Child window, visible
        0, 0, w, h,                 // Fill parent
        parent_hwnd_,               // Parent
        nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );

    if (!video_hwnd_) {
        std::cerr << "Failed to create video child window: " << GetLastError() << std::endl;
        return false;
    }

    // Position video window at bottom of Z-order (below other content)
    SetWindowPos(video_hwnd_, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    std::cerr << "Video child window created: " << w << "x" << h << std::endl;

    // Create Vulkan instance
    const char* instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_2;
    appInfo.pApplicationName = "Jellyfin Desktop";

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 3;
    instanceInfo.ppEnabledExtensionNames = instanceExts;

    if (vkCreateInstance(&instanceInfo, nullptr, &instance_) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance" << std::endl;
        return false;
    }

    // Select physical device
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance_, &gpuCount, nullptr);
    if (gpuCount == 0) {
        std::cerr << "No Vulkan devices found" << std::endl;
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance_, &gpuCount, gpus.data());
    physical_device_ = gpus[0];

    VkPhysicalDeviceProperties gpuProps;
    vkGetPhysicalDeviceProperties(physical_device_, &gpuProps);
    std::cerr << "WindowsVideoLayer using GPU: " << gpuProps.deviceName << std::endl;

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

    // Create device with required features
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
    deviceInfo.enabledExtensionCount = s_deviceExtensionCount;
    deviceInfo.ppEnabledExtensionNames = s_deviceExtensions;

    if (vkCreateDevice(physical_device_, &deviceInfo, nullptr, &device_) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan device" << std::endl;
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    device_extensions_ = s_deviceExtensions;
    device_extension_count_ = s_deviceExtensionCount;

    // Create Vulkan surface for child window
    VkWin32SurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    surfaceInfo.hwnd = video_hwnd_;

    auto vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)
        vkGetInstanceProcAddr(instance_, "vkCreateWin32SurfaceKHR");
    if (!vkCreateWin32SurfaceKHR ||
        vkCreateWin32SurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan Win32 surface" << std::endl;
        return false;
    }

    std::cerr << "Windows video layer initialized" << std::endl;
    return true;
}

bool WindowsVideoLayer::createSwapchain(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;

    // Query surface formats
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, formats.data());

    // Prefer HDR format if available
    format_ = VK_FORMAT_B8G8R8A8_UNORM;
    color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    is_hdr_ = false;

    // Try 10-bit HDR formats
    for (const auto& fmt : formats) {
        if (fmt.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            if (fmt.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                fmt.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
                format_ = fmt.format;
                color_space_ = fmt.colorSpace;
                is_hdr_ = true;
                std::cerr << "Using HDR format " << fmt.format << std::endl;
                break;
            }
        }
    }

    if (!is_hdr_) {
        std::cerr << "HDR not available, using SDR" << std::endl;
    }

    // Get surface capabilities
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

    // Create swapchain
    VkSwapchainCreateInfoKHR swapInfo{};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = surface_;
    swapInfo.minImageCount = std::max(2u, caps.minImageCount);
    swapInfo.imageFormat = format_;
    swapInfo.imageColorSpace = color_space_;
    swapInfo.imageExtent = {width, height};
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;
    swapInfo.oldSwapchain = swapchain_;

    if (vkCreateSwapchainKHR(device_, &swapInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        std::cerr << "Failed to create swapchain" << std::endl;
        return false;
    }

    // Get swapchain images
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, nullptr);
    image_count_ = std::min(image_count_, MAX_IMAGES);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, images_);

    // Create image views
    for (uint32_t i = 0; i < image_count_; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(device_, &viewInfo, nullptr, &image_views_[i]) != VK_SUCCESS) {
            std::cerr << "Failed to create image view " << i << std::endl;
            return false;
        }
    }

    // Create sync objects
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(device_, &semInfo, nullptr, &image_available_);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(device_, &fenceInfo, nullptr, &acquire_fence_);

    std::cerr << "Swapchain created: " << width << "x" << height
              << " format=" << format_ << " HDR=" << (is_hdr_ ? "yes" : "no") << std::endl;

    return true;
}

void WindowsVideoLayer::destroySwapchain() {
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

    for (uint32_t i = 0; i < image_count_; i++) {
        if (image_views_[i]) {
            vkDestroyImageView(device_, image_views_[i], nullptr);
            image_views_[i] = VK_NULL_HANDLE;
        }
    }
    image_count_ = 0;

    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool WindowsVideoLayer::startFrame(VkImage* outImage, VkImageView* outView, VkFormat* outFormat) {
    if (frame_active_ || !swapchain_) return false;

    vkResetFences(device_, 1, &acquire_fence_);
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                             VK_NULL_HANDLE, acquire_fence_, &current_image_idx_);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return false;
    }
    vkWaitForFences(device_, 1, &acquire_fence_, VK_TRUE, UINT64_MAX);

    frame_active_ = true;
    *outImage = images_[current_image_idx_];
    *outView = image_views_[current_image_idx_];
    *outFormat = format_;
    return true;
}

void WindowsVideoLayer::submitFrame() {
    if (!frame_active_) return;

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &current_image_idx_;

    vkQueuePresentKHR(queue_, &presentInfo);
    frame_active_ = false;
    visible_ = true;
}

void WindowsVideoLayer::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) return;

    // Resize child window
    if (video_hwnd_) {
        SetWindowPos(video_hwnd_, HWND_BOTTOM, 0, 0, width, height, SWP_NOACTIVATE);
    }

    vkDeviceWaitIdle(device_);
    destroySwapchain();
    createSwapchain(width, height);
}

void WindowsVideoLayer::setVisible(bool visible) {
    if (video_hwnd_) {
        ShowWindow(video_hwnd_, visible ? SW_SHOW : SW_HIDE);
    }
    visible_ = visible;
}

void WindowsVideoLayer::setPosition(int x, int y) {
    if (video_hwnd_) {
        SetWindowPos(video_hwnd_, HWND_BOTTOM, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void WindowsVideoLayer::cleanup() {
    destroySwapchain();

    if (surface_ && instance_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    if (video_hwnd_) {
        DestroyWindow(video_hwnd_);
        video_hwnd_ = nullptr;
    }
}

#endif // _WIN32
```

**Step 3: Commit**

```bash
git add src/platform/windows_video_layer.h src/platform/windows_video_layer.cpp
git commit -m "feat(windows): add Windows video layer with child HWND"
```

---

## Task 3: Update CMakeLists.txt for Windows

**Files:**
- Modify: `CMakeLists.txt`

**Step 1: Add Windows platform sources to CMakeLists.txt**

Add Windows platform section alongside existing Linux/macOS conditionals.

Edit `CMakeLists.txt` to add a Windows section after the Linux `else()` block (around line 63):

```cmake
# Platform-specific dependencies
if(APPLE)
    # ... existing macOS code ...
elseif(WIN32)
    # Windows: WGL for OpenGL
    find_package(OpenGL REQUIRED)

    set(PLATFORM_SOURCES
        src/context/wgl_context.cpp
        src/platform/windows_video_layer.cpp
        src/compositor/opengl_compositor.cpp
        src/player/media_session.cpp
    )
    set(PLATFORM_LIBRARIES
        OpenGL::GL
        dwmapi
    )
    set(PLATFORM_INCLUDE_DIRS "")
else()
    # Linux: OpenGL/EGL, Wayland (existing code)
    # ... keep existing Linux code ...
endif()
```

**Step 2: Commit**

```bash
git add CMakeLists.txt
git commit -m "build(cmake): add Windows platform support"
```

---

## Task 4: Update OpenGL Compositor for WGL

**Files:**
- Modify: `src/compositor/opengl_compositor.h`
- Modify: `src/compositor/opengl_compositor.cpp`

**Step 1: Add WGL context typedef to header**

Edit `src/compositor/opengl_compositor.h` to add Windows support at the top:

```cpp
#pragma once

#ifdef __APPLE__
#include "context/cgl_context.h"
#include <OpenGL/gl3.h>
#include <IOSurface/IOSurface.h>
typedef CGLContext GLContext;
#elif defined(_WIN32)
#include "context/wgl_context.h"
#include <GL/gl.h>
typedef WGLContext GLContext;
#else
#include "context/egl_context.h"
#include <libdrm/drm_fourcc.h>
typedef EGLContext_ GLContext;
#endif
```

**Step 2: Update cpp shader code for Windows**

Add Windows-specific shader code (GLSL 1.30 compatible) in `opengl_compositor.cpp` after the macOS shaders:

```cpp
#elif defined(_WIN32)
// Windows: Desktop OpenGL 3.0+ with GL_TEXTURE_2D
static const char* vert_src = R"(#version 130
out vec2 texCoord;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texCoord = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* frag_src = R"(#version 130
in vec2 texCoord;
out vec4 fragColor;
uniform sampler2D overlayTex;
uniform float alpha;
void main() {
    vec4 color = texture(overlayTex, texCoord);
    // CEF provides BGRA - swizzle to RGBA
    fragColor = color.bgra * alpha;
}
)";
```

**Step 3: Commit**

```bash
git add src/compositor/opengl_compositor.h src/compositor/opengl_compositor.cpp
git commit -m "feat(compositor): add WGL support for Windows"
```

---

## Task 5: Update MpvPlayerVk for Windows

**Files:**
- Modify: `src/player/mpv/mpv_player_vk.h`

**Step 1: Add Windows video surface typedef**

Edit `src/player/mpv/mpv_player_vk.h` to add Windows support:

```cpp
#pragma once

#include "context/vulkan_context.h"
#ifdef __APPLE__
#include "platform/macos_layer.h"
using VideoSurface = MacOSVideoLayer;
#elif defined(_WIN32)
#include "platform/windows_video_layer.h"
using VideoSurface = WindowsVideoLayer;
#else
#include "platform/wayland_subsurface.h"
using VideoSurface = WaylandSubsurface;
#endif
```

**Step 2: Commit**

```bash
git add src/player/mpv/mpv_player_vk.h
git commit -m "feat(mpv): add Windows video surface support"
```

---

## Task 6: Update main.cpp for Windows

**Files:**
- Modify: `src/main.cpp`

**Step 1: Add Windows includes and platform initialization**

Add Windows-specific includes after the existing platform includes (around line 27):

```cpp
#ifdef __APPLE__
// ... existing macOS includes ...
#elif defined(_WIN32)
#include "context/wgl_context.h"
#include "platform/windows_video_layer.h"
#include "compositor/opengl_compositor.h"
#include "player/media_session.h"
#else
// ... existing Linux includes ...
#endif
```

**Step 2: Add Windows initialization block in main()**

Add Windows platform initialization alongside existing macOS and Linux blocks (after line 359):

```cpp
#elif defined(_WIN32)
    // Windows: Initialize WGL context for OpenGL rendering
    WGLContext wgl;
    if (!wgl.init(window)) {
        std::cerr << "WGL init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize Windows video layer
    WindowsVideoLayer videoLayer;
    if (!videoLayer.init(window, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0,
                         nullptr, 0, nullptr)) {
        std::cerr << "Fatal: Windows video layer init failed" << std::endl;
        return 1;
    }
    if (!videoLayer.createSwapchain(width, height)) {
        std::cerr << "Fatal: Windows video layer swapchain failed" << std::endl;
        return 1;
    }
    bool has_subsurface = true;
    std::cerr << "Using Windows child window for video (HDR: "
              << (videoLayer.isHdr() ? "yes" : "no") << ")" << std::endl;

    // Initialize mpv player
    MpvPlayerVk mpv;
    bool has_video = false;
    bool video_needs_rerender = false;
    double current_playback_rate = 1.0;
    if (!mpv.init(nullptr, &videoLayer)) {
        std::cerr << "MpvPlayerVk init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize OpenGL compositor for CEF overlay
    OpenGLCompositor compositor;
    if (!compositor.init(&wgl, width, height, false)) {
        std::cerr << "OpenGLCompositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Second compositor for overlay browser
    OpenGLCompositor overlay_compositor;
    if (!overlay_compositor.init(&wgl, width, height, false)) {
        std::cerr << "Overlay compositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
```

**Step 3: Add Windows render loop section**

Add Windows-specific rendering code in the main loop (following the pattern of Linux):

```cpp
#elif defined(_WIN32)
        // Windows: Render video to child window
        if (has_subsurface && ((has_video && mpv.hasFrame()) || video_needs_rerender)) {
            VkImage sub_image;
            VkImageView sub_view;
            VkFormat sub_format;
            if (videoLayer.startFrame(&sub_image, &sub_view, &sub_format)) {
                mpv.render(sub_image, sub_view,
                          videoLayer.width(), videoLayer.height(),
                          sub_format);
                videoLayer.submitFrame();
                video_ready = true;
                video_needs_rerender = false;
            }
        }

        flushPaintBuffer();
        compositor.flushOverlay();

        // Clear and composite
        glClearColor(0.0f, 0.0f, 0.0f, video_ready ? 0.0f : 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (test_video.empty() && compositor.hasValidOverlay()) {
            float alpha = video_ready ? overlay_alpha : 1.0f;
            compositor.composite(current_width, current_height, alpha);
        }

        if (overlay_state != OverlayState::HIDDEN && overlay_browser_alpha > 0.01f) {
            overlay_compositor.flushOverlay();
            if (overlay_compositor.hasValidOverlay()) {
                overlay_compositor.composite(current_width, current_height, overlay_browser_alpha);
            }
        }

        wgl.swapBuffers();
```

**Step 4: Add Windows cleanup section**

Add Windows cleanup code at the end:

```cpp
#elif defined(_WIN32)
    if (has_subsurface) {
        videoLayer.cleanup();
    }
    wgl.cleanup();
```

**Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "feat(main): add Windows platform initialization and render loop"
```

---

## Task 7: Add Windows Media Session Stub

**Files:**
- Modify: `src/player/media_session.cpp`

**Step 1: Add Windows stub to media_session.cpp**

The media session file needs a Windows backend. For initial Windows support, add a stub:

```cpp
#ifdef _WIN32
// Windows: SMTC backend (stub for now)
std::unique_ptr<MediaSessionBackend> createWindowsMediaBackend(MediaSession*) {
    return nullptr;  // TODO: Implement Windows SMTC
}
#endif
```

**Step 2: Commit**

```bash
git add src/player/media_session.cpp
git commit -m "feat(media): add Windows media session stub"
```

---

## Task 8: Test Build on Windows

**Step 1: Configure CMake on Windows**

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
```

Expected: Configuration succeeds, finds Vulkan SDK and WGL.

**Step 2: Build**

```powershell
cmake --build build --config Release
```

Expected: Build succeeds with no errors.

**Step 3: Commit final changes if any fixups needed**

```bash
git add -A
git commit -m "fix(windows): build fixes for Windows support"
```

---

## Summary

This plan adds Windows support through:

1. **WGL Context** - OpenGL context for CEF compositor rendering
2. **Windows Video Layer** - Child HWND with Vulkan swapchain for mpv
3. **CMake Updates** - Platform detection and Windows-specific sources
4. **Compositor Updates** - WGL support in OpenGL compositor
5. **main.cpp Updates** - Windows initialization and render loop

The architecture mirrors the existing Linux (Wayland subsurface) and macOS (CAMetalLayer) implementations:
- Video renders to a separate window/layer positioned below the main content
- CEF overlay renders with transparency on top
- Both layers composite through the window system

## Open Questions for User

1. **HDR Support Priority**: Windows HDR requires DXGI swapchain hints. Should HDR support be included in initial implementation or deferred?

2. **SMTC Media Session**: Windows System Media Transport Controls integration - implement now or stub?

3. **DX Interop**: CEF on Windows can share textures via DX11 interop for zero-copy. Worth implementing for GPU path?

4. **Child Window Z-ordering**: The child HWND approach may have issues with window occlusion. Should we investigate DirectComposition as an alternative?
