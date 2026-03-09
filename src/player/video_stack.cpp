#include "video_stack.h"
#include "video_renderer.h"
#include "mpv/mpv_player.h"
#include "logging.h"
#include <SDL3/SDL.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <cstdlib>

// Shared Vulkan pre-init hook: sets hwdec-codecs and PQ/BT.2020 HDR options
static void configureVulkanHook(mpv_handle* mpv, bool use_hdr) {
    mpv_set_option_string(mpv, "hwdec-codecs", "h264,vc1,hevc,vp8,av1,prores,prores_raw,ffv1,dpx");
    if (use_hdr) {
#ifdef __APPLE__
        // macOS EDR uses extended linear sRGB - output linear light values
        mpv_set_option_string(mpv, "target-prim", "bt.709");
        mpv_set_option_string(mpv, "target-trc", "linear");
        mpv_set_option_string(mpv, "tone-mapping", "clip");
        double peak = 1000.0;  // EDR headroom
        mpv_set_option(mpv, "target-peak", MPV_FORMAT_DOUBLE, &peak);
        LOG_INFO(LOG_MPV, "HDR output enabled (bt.709/linear for macOS EDR)");
#else
        // Linux Wayland and Windows HDR use PQ/BT.2020
        mpv_set_option_string(mpv, "target-prim", "bt.2020");
        mpv_set_option_string(mpv, "target-trc", "pq");
        mpv_set_option_string(mpv, "target-colorspace-hint", "yes");
        mpv_set_option_string(mpv, "tone-mapping", "clip");  // No tone mapping for passthrough
        double peak = 1000.0;
        mpv_set_option(mpv, "target-peak", MPV_FORMAT_DOUBLE, &peak);
        LOG_INFO(LOG_MPV, "HDR output enabled (bt.2020/pq/1000 nits)");
#endif
    }
}

#ifdef __APPLE__
#include <mpv/render_vk.h>
#include "platform/macos_layer.h"
#include "vulkan_subsurface_renderer.h"

template<typename Surface>
static bool createVulkanRenderContext(MpvPlayer* player, Surface* surface) {
    mpv_vulkan_init_params vk_params{};
    vk_params.instance = surface->vkInstance();
    vk_params.physical_device = surface->vkPhysicalDevice();
    vk_params.device = surface->vkDevice();
    vk_params.graphics_queue = surface->vkQueue();
    vk_params.graphics_queue_family = surface->vkQueueFamily();
    vk_params.get_instance_proc_addr = surface->vkGetProcAddr();
    vk_params.features = surface->features();
    vk_params.extensions = surface->deviceExtensions();
    vk_params.num_extensions = surface->deviceExtensionCount();

    int advanced_control = 1;
    const char* backends[] = {"gpu-next", "gpu"};
    for (const char* backend : backends) {
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_VULKAN)},
            {MPV_RENDER_PARAM_BACKEND, const_cast<char*>(backend)},
            {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vk_params},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };
        if (player->createRenderContext(params)) {
            LOG_INFO(LOG_MPV, "Using backend: %s", backend);
            return true;
        }
        LOG_WARN(LOG_MPV, "Backend '%s' failed, trying next", backend);
    }
    LOG_ERROR(LOG_MPV, "All Vulkan backends failed");
    return false;
}

// Internal storage for macOS video layer (must outlive renderer)
namespace {
    std::unique_ptr<MacOSVideoLayer> g_macos_layer;
    bool atexit_registered = false;

    void atexitCleanup() {
        // Clean up before static destructors run (handles Cmd+Q via NSApplication terminate:)
        if (g_macos_layer) {
            g_macos_layer->cleanup();
            g_macos_layer.reset();
        }
    }
}

VideoStack VideoStack::create(SDL_Window* window, int width, int height, const char* hwdec) {
    VideoStack stack;

    // Register atexit handler to clean up before static destructors
    // (handles Cmd+Q via NSApplication terminate: which calls exit())
    if (!atexit_registered) {
        std::atexit(atexitCleanup);
        atexit_registered = true;
    }

    // Get physical dimensions for HiDPI
    int physical_w, physical_h;
    SDL_GetWindowSizeInPixels(window, &physical_w, &physical_h);

    // Create video layer
    g_macos_layer = std::make_unique<MacOSVideoLayer>();
    if (!g_macos_layer->init(window, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0,
                             nullptr, 0, nullptr)) {
        LOG_ERROR(LOG_PLATFORM, "Fatal: macOS video layer init failed");
        return stack;
    }
    if (!g_macos_layer->createSwapchain(physical_w, physical_h)) {
        LOG_ERROR(LOG_PLATFORM, "Fatal: macOS video layer swapchain failed");
        return stack;
    }
    LOG_INFO(LOG_PLATFORM, "Using macOS CAMetalLayer for video (HDR: %s)",
             g_macos_layer->isHdr() ? "yes" : "no");

    // Create player with HDR pre-init hook
    auto player = std::make_unique<MpvPlayer>();
    bool use_hdr = g_macos_layer->isHdr();
    if (!player->init(hwdec, [&](mpv_handle* mpv) {
        configureVulkanHook(mpv, use_hdr);
    })) {
        LOG_ERROR(LOG_MPV, "MpvPlayer init failed");
        return stack;
    }

    if (!createVulkanRenderContext(player.get(), g_macos_layer.get())) {
        return stack;
    }
    LOG_INFO(LOG_MPV, "Vulkan render context created");

    // Create renderer
    stack.renderer = std::make_unique<VulkanSubsurfaceRenderer>(player.get(), g_macos_layer.get());
    stack.player = std::move(player);

    return stack;
}

#elif defined(_WIN32)
#include <mpv/render_vk.h>
#include "vulkan_subsurface_renderer.h"
#include "platform/windows_video_surface.h"

template<typename Surface>
static bool createVulkanRenderContext(MpvPlayer* player, Surface* surface) {
    mpv_vulkan_init_params vk_params{};
    vk_params.instance = surface->vkInstance();
    vk_params.physical_device = surface->vkPhysicalDevice();
    vk_params.device = surface->vkDevice();
    vk_params.graphics_queue = surface->vkQueue();
    vk_params.graphics_queue_family = surface->vkQueueFamily();
    vk_params.get_instance_proc_addr = surface->vkGetProcAddr();
    vk_params.features = surface->features();
    vk_params.extensions = surface->deviceExtensions();
    vk_params.num_extensions = surface->deviceExtensionCount();

    int advanced_control = 1;
    const char* backends[] = {"gpu-next", "gpu"};
    for (const char* backend : backends) {
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_VULKAN)},
            {MPV_RENDER_PARAM_BACKEND, const_cast<char*>(backend)},
            {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vk_params},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };
        if (player->createRenderContext(params)) {
            LOG_INFO(LOG_MPV, "Using backend: %s", backend);
            return true;
        }
        LOG_WARN(LOG_MPV, "Backend '%s' failed, trying next", backend);
    }
    LOG_ERROR(LOG_MPV, "All Vulkan backends failed");
    return false;
}

namespace {
    std::unique_ptr<WindowsVideoSurface> g_windows_video_surface;
    bool atexit_registered = false;

    void atexitCleanup() {
        if (g_windows_video_surface) {
            g_windows_video_surface->cleanup();
            g_windows_video_surface.reset();
        }
    }
}

VideoStack VideoStack::create(SDL_Window* window, int width, int height, const char* hwdec) {
    (void)width; (void)height;  // Use physical dimensions instead
    VideoStack stack;

    if (!atexit_registered) {
        std::atexit(atexitCleanup);
        atexit_registered = true;
    }

    g_windows_video_surface = std::make_unique<WindowsVideoSurface>();
    if (!g_windows_video_surface->init(window, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0,
                                       nullptr, 0, nullptr)) {
        LOG_ERROR(LOG_PLATFORM, "Fatal: Windows video surface init failed");
        return stack;
    }

    int physical_w, physical_h;
    SDL_GetWindowSizeInPixels(window, &physical_w, &physical_h);
    if (!g_windows_video_surface->createSwapchain(physical_w, physical_h)) {
        LOG_ERROR(LOG_PLATFORM, "Fatal: Windows video surface swapchain failed");
        return stack;
    }

    auto player = std::make_unique<MpvPlayer>();
    bool use_hdr = g_windows_video_surface->isHdr();
    if (!player->init(hwdec, [&](mpv_handle* mpv) {
        configureVulkanHook(mpv, use_hdr);
    })) {
        LOG_ERROR(LOG_MPV, "MpvPlayer init failed");
        return stack;
    }

    if (!createVulkanRenderContext(player.get(), g_windows_video_surface.get())) {
        return stack;
    }
    LOG_INFO(LOG_MPV, "Vulkan render context created");

    stack.renderer = std::make_unique<VulkanSubsurfaceRenderer>(player.get(), g_windows_video_surface.get());
    stack.player = std::move(player);
    stack.surface = g_windows_video_surface.get();

    LOG_INFO(LOG_PLATFORM, "Using Vulkan gpu-next with DComp for video (HDR: %s)",
             g_windows_video_surface->isHdr() ? "yes" : "no");
    return stack;
}

#else // Linux
#include <mpv/render_vk.h>
#include <mpv/render_gl.h>
#include "platform/wayland_subsurface.h"
#include "context/egl_context.h"
#include "vulkan_subsurface_renderer.h"
#include "opengl_renderer.h"
#include <cstring>

template<typename Surface>
static bool createVulkanRenderContext(MpvPlayer* player, Surface* surface) {
    mpv_vulkan_init_params vk_params{};
    vk_params.instance = surface->vkInstance();
    vk_params.physical_device = surface->vkPhysicalDevice();
    vk_params.device = surface->vkDevice();
    vk_params.graphics_queue = surface->vkQueue();
    vk_params.graphics_queue_family = surface->vkQueueFamily();
    vk_params.get_instance_proc_addr = surface->vkGetProcAddr();
    vk_params.features = surface->features();
    vk_params.extensions = surface->deviceExtensions();
    vk_params.num_extensions = surface->deviceExtensionCount();

    int advanced_control = 1;
    const char* backends[] = {"gpu-next", "gpu"};
    for (const char* backend : backends) {
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_VULKAN)},
            {MPV_RENDER_PARAM_BACKEND, const_cast<char*>(backend)},
            {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vk_params},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };
        if (player->createRenderContext(params)) {
            LOG_INFO(LOG_MPV, "Using backend: %s", backend);
            return true;
        }
        LOG_WARN(LOG_MPV, "Backend '%s' failed, trying next", backend);
    }
    LOG_ERROR(LOG_MPV, "All Vulkan backends failed");
    return false;
}

// Internal storage for Wayland subsurface (must outlive renderer)
namespace {
    std::unique_ptr<WaylandSubsurface> g_wayland_subsurface;
}

static void* gl_get_proc_address(void* ctx, const char* name) {
    (void)ctx;
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

VideoStack VideoStack::create(SDL_Window* window, int width, int height, EGLContext_* egl, const char* hwdec) {
    VideoStack stack;

    // Detect Wayland vs X11 at runtime
    const char* videoDriver = SDL_GetCurrentVideoDriver();
    bool useWayland = videoDriver && strcmp(videoDriver, "wayland") == 0;
    LOG_INFO(LOG_MAIN, "SDL video driver: %s -> using %s",
             videoDriver ? videoDriver : "null", useWayland ? "Wayland" : "X11");

    if (useWayland) {
        // Wayland: Vulkan subsurface for HDR
        g_wayland_subsurface = std::make_unique<WaylandSubsurface>();
        if (!g_wayland_subsurface->init(window, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0,
                                        nullptr, 0, nullptr)) {
            LOG_ERROR(LOG_PLATFORM, "Fatal: Wayland subsurface init failed");
            return stack;
        }

        int physical_w, physical_h;
        SDL_GetWindowSizeInPixels(window, &physical_w, &physical_h);
        if (!g_wayland_subsurface->createSwapchain(physical_w, physical_h)) {
            LOG_ERROR(LOG_PLATFORM, "Fatal: Wayland subsurface swapchain failed");
            return stack;
        }
        g_wayland_subsurface->initDestinationSize(width, height);
        LOG_INFO(LOG_PLATFORM, "Using Wayland subsurface for video (HDR: %s)",
                 g_wayland_subsurface->isHdr() ? "yes" : "no");

        auto player = std::make_unique<MpvPlayer>();
        bool use_hdr = g_wayland_subsurface->isHdr();
        if (!player->init(hwdec, [&](mpv_handle* mpv) {
            configureVulkanHook(mpv, use_hdr);
        })) {
            LOG_ERROR(LOG_MPV, "MpvPlayer init failed");
            return stack;
        }

        if (!createVulkanRenderContext(player.get(), g_wayland_subsurface.get())) {
            return stack;
        }
        LOG_INFO(LOG_MPV, "Vulkan render context created");

        stack.renderer = std::make_unique<VulkanSubsurfaceRenderer>(player.get(), g_wayland_subsurface.get());
        stack.player = std::move(player);
    } else {
        // X11: OpenGL composition with threaded rendering
        auto player = std::make_unique<MpvPlayer>();
        if (!player->init(hwdec)) {
            LOG_ERROR(LOG_MPV, "MpvPlayer init failed");
            return stack;
        }

        // Build OpenGL render context params
        mpv_opengl_init_params gl_init{};
        gl_init.get_proc_address = gl_get_proc_address;
        gl_init.get_proc_address_ctx = egl;

        int advanced_control = 1;

#ifdef MPV_RENDER_PARAM_BACKEND
        const char* backend = "gpu-next";
#endif

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
#ifdef MPV_RENDER_PARAM_BACKEND
            {MPV_RENDER_PARAM_BACKEND, const_cast<char*>(backend)},
#endif
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };

        if (!player->createRenderContext(params)) {
            LOG_ERROR(LOG_MPV, "OpenGL render context creation failed");
            return stack;
        }
        LOG_INFO(LOG_MPV, "mpv OpenGL render context created");

        auto renderer = std::make_unique<OpenGLRenderer>(player.get());
        if (!renderer->initThreaded(egl)) {
            LOG_ERROR(LOG_VIDEO, "OpenGLRenderer threaded init failed");
            return stack;
        }

        stack.renderer = std::move(renderer);
        stack.player = std::move(player);

        LOG_INFO(LOG_PLATFORM, "Using OpenGL composition for video (X11, threaded)");
    }

    return stack;
}

#endif

void VideoStack::cleanupStatics() {
#ifdef __APPLE__
    if (g_macos_layer) {
        g_macos_layer->cleanup();
        g_macos_layer.reset();
    }
#elif defined(_WIN32)
    if (g_windows_video_surface) {
        g_windows_video_surface->cleanup();
        g_windows_video_surface.reset();
    }
#else
    if (g_wayland_subsurface) {
        g_wayland_subsurface->cleanup();
        g_wayland_subsurface.reset();
    }
#endif
}
