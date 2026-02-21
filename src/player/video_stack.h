#pragma once

#include <memory>

class MpvPlayer;
class VideoRenderer;
struct SDL_Window;

#ifdef _WIN32
class WGLContext;
#elif !defined(__APPLE__)
class EGLContext_;
#endif

// Video subsystem - owns player and renderer
struct VideoStack {
    std::unique_ptr<MpvPlayer> player;
    std::unique_ptr<VideoRenderer> renderer;

    // Factory - creates platform-appropriate video stack
#ifdef __APPLE__
    static VideoStack create(SDL_Window* window, int width, int height, const char* hwdec = "auto-safe");
#elif defined(_WIN32)
    static VideoStack create(SDL_Window* window, int width, int height, WGLContext* wgl, const char* hwdec = "auto-safe");
#else
    static VideoStack create(SDL_Window* window, int width, int height, EGLContext_* egl, const char* hwdec = "auto-safe");
#endif

    // Cleanup static resources (call before program exit to avoid static destructor issues)
    static void cleanupStatics();
};
