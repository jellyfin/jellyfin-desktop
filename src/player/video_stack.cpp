#include "video_stack.h"
#include "video_renderer.h"
#include "mpv/mpv_player.h"
#include "logging.h"
#include <SDL3/SDL.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_vk.h>
#include <cstdlib>

// Creates a Vulkan render context with a libplacebo-managed swapchain.
// Used by platforms where libplacebo owns the swapchain (macOS, Windows,
// Wayland fallback when compositor lacks parametric color management).
// display_profile is only passed when the platform manages color (isHdr) —
// without it, Mesa's WSI handles color management and passing a profile
// would cause libmpv to override Mesa's sRGB default with HDR hints.
template<typename Surface>
static bool createSwapchainRenderContext(MpvPlayer* player, Surface* surface) {
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

    VkSurfaceKHR vk_surface = surface->vkSurface();
    auto* dp = const_cast<mpv_display_profile*>(&surface->displayProfile());
    int advanced_control = 1;
    const char* backend = "gpu-next";
    bool use_profile = surface->isHdr();
    mpv_render_param params_with_profile[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_VULKAN)},
        {MPV_RENDER_PARAM_BACKEND, const_cast<char*>(backend)},
        {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vk_params},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {MPV_RENDER_PARAM_VULKAN_SURFACE, &vk_surface},
        {MPV_RENDER_PARAM_DISPLAY_PROFILE, dp},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    mpv_render_param params_no_profile[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_VULKAN)},
        {MPV_RENDER_PARAM_BACKEND, const_cast<char*>(backend)},
        {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vk_params},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {MPV_RENDER_PARAM_VULKAN_SURFACE, &vk_surface},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    auto* params = use_profile ? params_with_profile : params_no_profile;
    if (!player->createRenderContext(params)) {
        LOG_ERROR(LOG_MPV, "Failed to create render context with VkSurface");
        return false;
    }
    if (use_profile && dp->swapchain_out)
        surface->setSwapchain(dp->swapchain_out);
    LOG_INFO(LOG_MPV, "Vulkan render context created (libplacebo swapchain)");
    return true;
}

// Creates a Vulkan render context WITHOUT a swapchain (FBO-only mode).
// Used for dmabuf presentation where we manage the surface ourselves.
template<typename Surface>
static bool createFboRenderContext(MpvPlayer* player, Surface* surface) {
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

    // Pass display profile for content peak communication.
    // video.c does PQ passthrough (no tone mapping) and writes the source
    // content's peak luminance to display_profile.content_peak. The platform
    // layer reads it to update the surface image description with the actual
    // content metadata — matching standalone mpv's set_color_management.
    auto* dp = const_cast<mpv_display_profile*>(&surface->displayProfile());
    int advanced_control = 1;
    const char* backend = "gpu-next";
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_VULKAN)},
        {MPV_RENDER_PARAM_BACKEND, const_cast<char*>(backend)},
        {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vk_params},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {MPV_RENDER_PARAM_DISPLAY_PROFILE, dp},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    if (!player->createRenderContext(params)) {
        LOG_ERROR(LOG_MPV, "Failed to create render context (FBO mode)");
        return false;
    }
    LOG_INFO(LOG_MPV, "Vulkan render context created (FBO mode, no swapchain)");
    return true;
}

static void configureAudioOptions(mpv_handle* mpv, const AudioConfig& audio) {
    if (audio.spdif && audio.spdif[0])
        mpv_set_option_string(mpv, "audio-spdif", audio.spdif);
    if (audio.channels && audio.channels[0])
        mpv_set_option_string(mpv, "audio-channels", audio.channels);
    if (audio.exclusive)
        mpv_set_option_string(mpv, "audio-exclusive", "yes");
}

static void configureVulkanHook(mpv_handle* mpv, bool /*use_hdr*/) {
    mpv_set_option_string(mpv, "hwdec-codecs", "h264,vc1,hevc,vp8,av1,prores,prores_raw,ffv1,dpx");
}

#ifdef __APPLE__
#include "platform/macos_layer.h"
#include "vulkan_subsurface_renderer.h"

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

VideoStack VideoStack::create(SDL_Window* window, int width, int height, const char* hwdec, AudioConfig audio) {
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

    // Create video layer (sets up CAMetalLayer + Vulkan instance/device/surface)
    g_macos_layer = std::make_unique<MacOSVideoLayer>();
    if (!g_macos_layer->init(window, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0,
                             nullptr, 0, nullptr)) {
        LOG_ERROR(LOG_PLATFORM, "Fatal: macOS video layer init failed");
        return stack;
    }
    if (!g_macos_layer->createSwapchain(physical_w, physical_h)) {
        LOG_ERROR(LOG_PLATFORM, "Fatal: macOS video layer init failed");
        return stack;
    }
    LOG_INFO(LOG_PLATFORM, "Using macOS CAMetalLayer for video (HDR: %s)",
             g_macos_layer->isHdr() ? "yes" : "no");

    // Create player with HDR pre-init hook
    auto player = std::make_unique<MpvPlayer>();
    bool use_hdr = g_macos_layer->isHdr();
    if (!player->init(hwdec, [&](mpv_handle* mpv) {
        configureVulkanHook(mpv, use_hdr);
        configureAudioOptions(mpv, audio);
    })) {
        LOG_ERROR(LOG_MPV, "MpvPlayer init failed");
        return stack;
    }

    if (!createSwapchainRenderContext(player.get(), g_macos_layer.get())) {
        return stack;
    }

    // Create renderer
    stack.renderer = std::make_unique<VulkanSubsurfaceRenderer>(player.get(), g_macos_layer.get());
    stack.player = std::move(player);

    return stack;
}

#elif defined(_WIN32)
#include "vulkan_subsurface_renderer.h"
#include "platform/windows_video_layer.h"

namespace {
    std::unique_ptr<WindowsVideoLayer> g_windows_video_layer;
    bool atexit_registered = false;

    void atexitCleanup() {
        if (g_windows_video_layer) {
            g_windows_video_layer->cleanup();
            g_windows_video_layer.reset();
        }
    }
}

VideoStack VideoStack::create(SDL_Window* window, int width, int height, const char* hwdec, AudioConfig audio) {
    (void)width; (void)height;
    VideoStack stack;

    if (!atexit_registered) {
        std::atexit(atexitCleanup);
        atexit_registered = true;
    }

    g_windows_video_layer = std::make_unique<WindowsVideoLayer>();
    if (!g_windows_video_layer->init(window)) {
        LOG_ERROR(LOG_PLATFORM, "Fatal: Windows video layer init failed");
        return stack;
    }

    int physical_w, physical_h;
    SDL_GetWindowSizeInPixels(window, &physical_w, &physical_h);
    g_windows_video_layer->createSwapchain(physical_w, physical_h);

    auto player = std::make_unique<MpvPlayer>();
    bool use_hdr = g_windows_video_layer->isHdr();
    if (!player->init(hwdec, [&](mpv_handle* mpv) {
        configureVulkanHook(mpv, use_hdr);
        configureAudioOptions(mpv, audio);
    })) {
        LOG_ERROR(LOG_MPV, "MpvPlayer init failed");
        return stack;
    }

    if (!createSwapchainRenderContext(player.get(), g_windows_video_layer.get())) {
        return stack;
    }

    stack.renderer = std::make_unique<VulkanSubsurfaceRenderer>(player.get(), g_windows_video_layer.get());
    stack.player = std::move(player);
    stack.video_layer = g_windows_video_layer.get();

    LOG_INFO(LOG_PLATFORM, "Using Vulkan gpu-next with libplacebo swapchain (HDR: %s)",
             g_windows_video_layer->isHdr() ? "yes" : "no");
    return stack;
}

#else // Linux
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

VideoStack VideoStack::create(SDL_Window* window, int width, int height, EGLContext_* egl, const char* hwdec, AudioConfig audio) {
    VideoStack stack;

    // Detect Wayland vs X11 at runtime
    const char* videoDriver = SDL_GetCurrentVideoDriver();
    bool useWayland = videoDriver && strcmp(videoDriver, "wayland") == 0;
    LOG_INFO(LOG_MAIN, "SDL video driver: %s -> using %s",
             videoDriver ? videoDriver : "null", useWayland ? "Wayland" : "X11");

    if (useWayland) {
        // Wayland: Vulkan subsurface with dmabuf FBO + explicit color management
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
        // Dmabuf path when compositor supports wp_color_management parametric
        // descriptions. We own the surface color management for both HDR (PQ)
        // and SDR (sRGB), matching standalone mpv's explicit protocol path.
        // Only fall back to the swapchain path when the compositor lacks
        // parametric support (rare — KDE 6.x, wlroots 0.18+ all have it).
        bool will_dmabuf = g_wayland_subsurface->supports_parametric_;
        if (!player->init(hwdec, [&](mpv_handle* mpv) {
            configureVulkanHook(mpv, use_hdr);
            configureAudioOptions(mpv, audio);
            if (will_dmabuf) {
                if (use_hdr) {
                    mpv_set_option_string(mpv, "target-prim", "bt.2020");
                    mpv_set_option_string(mpv, "target-trc", "pq");
                } else {
                    mpv_set_option_string(mpv, "target-prim", "bt.709");
                    mpv_set_option_string(mpv, "target-trc", "srgb");
                }
            }
        })) {
            LOG_ERROR(LOG_MPV, "MpvPlayer init failed");
            return stack;
        }

        // Try dmabuf presentation path (explicit color management for HDR and SDR).
        // No Vulkan swapchain — we own the surface image description.
        bool use_dmabuf = will_dmabuf && g_wayland_subsurface->initDmabufPool(physical_w, physical_h);
        if (use_dmabuf) {
            // Activate color management on the parent surface so KWin sends
            // preferred_changed events when EDR/HDR mode toggles.
            g_wayland_subsurface->activateColorManagement();
            LOG_INFO(LOG_PLATFORM, "Using dmabuf presentation (%s)", use_hdr ? "HDR" : "SDR");
            if (!createFboRenderContext(player.get(), g_wayland_subsurface.get())) {
                return stack;
            }
        } else {
            // Swapchain fallback — compositor lacks parametric color management.
            // Mesa's WSI handles color management implicitly.
            if (!createSwapchainRenderContext(player.get(), g_wayland_subsurface.get())) {
                return stack;
            }
        }

        stack.renderer = std::make_unique<VulkanSubsurfaceRenderer>(player.get(), g_wayland_subsurface.get());
        stack.player = std::move(player);
    } else {
        // X11: OpenGL composition with threaded rendering
        auto player = std::make_unique<MpvPlayer>();
        if (!player->init(hwdec, [&](mpv_handle* mpv) {
            configureAudioOptions(mpv, audio);
        })) {
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
    if (g_windows_video_layer) {
        g_windows_video_layer->cleanup();
        g_windows_video_layer.reset();
    }
#else
    if (g_wayland_subsurface) {
        g_wayland_subsurface->cleanup();
        g_wayland_subsurface.reset();
    }
#endif
}
