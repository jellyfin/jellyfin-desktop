#include "video_stack.h"
#include "video_renderer.h"
#include "mpv/mpv_player.h"
#include "logging.h"
#include <SDL3/SDL.h>
#include <cstdlib>

#ifdef __APPLE__
#include "platform/macos_layer.h"
#include "mpv/mpv_player_vk.h"
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

    // Create player
    auto player = std::make_unique<MpvPlayerVk>();
    if (!player->init(nullptr, g_macos_layer.get(), hwdec)) {
        LOG_ERROR(LOG_MPV, "MpvPlayerVk init failed");
        return stack;
    }

    // Create renderer
    stack.renderer = std::make_unique<VulkanSubsurfaceRenderer>(player.get(), g_macos_layer.get());
    stack.player = std::move(player);

    return stack;
}

#elif defined(_WIN32)
#include "mpv/mpv_player_vk.h"
#include "vulkan_subsurface_renderer.h"
#include "platform/windows_video_surface.h"

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

    auto player = std::make_unique<MpvPlayerVk>();
    if (!player->init(nullptr, g_windows_video_surface.get(), hwdec)) {
        LOG_ERROR(LOG_MPV, "MpvPlayerVk init failed");
        return stack;
    }

    stack.renderer = std::make_unique<VulkanSubsurfaceRenderer>(player.get(), g_windows_video_surface.get());
    stack.player = std::move(player);

    LOG_INFO(LOG_PLATFORM, "Using Vulkan gpu-next with DComp for video (Windows)");
    return stack;
}

#else // Linux
#include "platform/wayland_subsurface.h"
#include "context/egl_context.h"
#include "mpv/mpv_player_vk.h"
#include "mpv/mpv_player_gl.h"
#include "vulkan_subsurface_renderer.h"
#include "opengl_renderer.h"
#include <cstring>

// Internal storage for Wayland subsurface (must outlive renderer)
namespace {
    std::unique_ptr<WaylandSubsurface> g_wayland_subsurface;
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

        auto player = std::make_unique<MpvPlayerVk>();
        if (!player->init(nullptr, g_wayland_subsurface.get(), hwdec)) {
            LOG_ERROR(LOG_MPV, "MpvPlayerVk init failed");
            return stack;
        }

        stack.renderer = std::make_unique<VulkanSubsurfaceRenderer>(player.get(), g_wayland_subsurface.get());
        stack.player = std::move(player);
    } else {
        // X11: OpenGL composition with threaded rendering
        auto player = std::make_unique<MpvPlayerGL>();
        if (!player->init(egl, hwdec)) {
            LOG_ERROR(LOG_MPV, "MpvPlayerGL init failed");
            return stack;
        }

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
