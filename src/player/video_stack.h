#pragma once

#include <memory>

class MpvPlayer;
class VideoRenderer;
struct SDL_Window;

#if !defined(__APPLE__) && !defined(_WIN32)
class EGLContext_;
#endif

#ifdef _WIN32
class WindowsVideoLayer;
#elif !defined(__APPLE__)
class WaylandSubsurface;
#endif

struct AudioConfig {
    const char* spdif = nullptr;    // --audio-passthrough codec list
    const char* channels = nullptr; // --audio-channels layout
    bool exclusive = false;         // --audio-exclusive
};

// Video subsystem - owns player and renderer
struct VideoStack {
    std::unique_ptr<MpvPlayer> player;
    std::unique_ptr<VideoRenderer> renderer;
#ifdef _WIN32
    WindowsVideoLayer* video_layer = nullptr;  // non-owning, for adapter LUID access
#elif !defined(__APPLE__)
    WaylandSubsurface* wayland_surface = nullptr;  // non-owning, for CEF subsurface access
#endif

    // Factory - creates platform-appropriate video stack
#if defined(__APPLE__) || defined(_WIN32)
    static VideoStack create(SDL_Window* window, int width, int height, const char* hwdec = "auto-safe", AudioConfig audio = {});
#else
    static VideoStack create(SDL_Window* window, int width, int height, EGLContext_* egl, const char* hwdec = "auto-safe", AudioConfig audio = {});
#endif

    // Cleanup static resources (call before program exit to avoid static destructor issues)
    static void cleanupStatics();
};
