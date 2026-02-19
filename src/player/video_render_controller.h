#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

class VideoRenderer;

// Handles video rendering - either on dedicated thread (Wayland/Vulkan) or
// synchronously on main thread (X11/OpenGL where context isn't shareable)
class VideoRenderController {
public:
    VideoRenderController() = default;
    ~VideoRenderController();

    // Initialize for threaded mode (Wayland/Vulkan - has own context)
    void startThreaded(VideoRenderer* renderer);

    // Initialize for synchronous mode (X11/OpenGL - must use main thread)
    void startSync(VideoRenderer* renderer);

    void stop();

    // Render video frame - threaded: updates dimensions, sync: renders directly
    void render(int width, int height);

    // Get clear alpha based on video ready state
    float getClearAlpha() const;

    // Request resize (executed on render thread, or immediately in sync mode)
    void requestResize(int width, int height);

    // Request colorspace setup (executed on render thread, or immediately in sync mode)
    void requestSetColorspace();

    // Enable/disable rendering
    void setActive(bool active) {
        active_.store(active);
        if (active) notify();
    }

    // Wake thread to check for new frames (called from mpv redraw callback)
    void notify() {
        frame_notified_.store(true);
        if (threaded_) cv_.notify_one();
    }

    // Query if video has been rendered at least once
    bool isVideoReady() const { return video_ready_.load(); }

    // Reset video ready state (e.g., when stopping video)
    void resetVideoReady() { video_ready_.store(false); }

private:
    void threadFunc();

    VideoRenderer* renderer_ = nullptr;
    std::thread thread_;
    bool threaded_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> video_ready_{false};
    std::atomic<bool> colorspace_pending_{false};
    std::atomic<bool> frame_notified_{false};

    // Dimensions - updated atomically by main thread, read by video thread
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};

    // Resize request (protected by resize_mutex_)
    std::mutex resize_mutex_;
    std::atomic<bool> resize_pending_{false};
    int resize_width_ = 0;
    int resize_height_ = 0;

    // Frame ready notification (threaded mode only)
    std::mutex cv_mutex_;
    std::condition_variable cv_;
};
