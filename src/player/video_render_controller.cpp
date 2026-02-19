#include "video_render_controller.h"
#include "video_renderer.h"
#include "logging.h"
#include <chrono>

VideoRenderController::~VideoRenderController() {
    stop();
}

void VideoRenderController::startThreaded(VideoRenderer* renderer) {
    renderer_ = renderer;
    threaded_ = true;
    running_.store(true);
    thread_ = std::thread(&VideoRenderController::threadFunc, this);
    LOG_INFO(LOG_VIDEO, "video render thread started (threaded mode)");
}

void VideoRenderController::startSync(VideoRenderer* renderer) {
    renderer_ = renderer;
    threaded_ = false;
    running_.store(true);
    LOG_INFO(LOG_VIDEO, "video render thread started (sync mode)");
}

void VideoRenderController::stop() {
    if (!running_.load()) return;

    running_.store(false);
    if (threaded_) {
        cv_.notify_one();  // Wake thread so it can exit
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    LOG_INFO(LOG_VIDEO, "video render thread stopped");
}

void VideoRenderController::render(int width, int height) {
    if (threaded_) {
        // Threaded: just update dimensions, background thread does rendering
        width_.store(width);
        height_.store(height);
    } else {
        // Sync: render directly on calling thread
        if (active_.load() && (renderer_->hasFrame() || video_ready_.load())) {
            renderer_->render(width, height);
            video_ready_.store(true);
        }
    }
}

float VideoRenderController::getClearAlpha() const {
    return renderer_->getClearAlpha(video_ready_.load());
}

void VideoRenderController::requestResize(int width, int height) {
    if (threaded_) {
        {
            std::lock_guard<std::mutex> lock(resize_mutex_);
            resize_width_ = width;
            resize_height_ = height;
        }
        resize_pending_.store(true);
        cv_.notify_one();
    } else {
        // Sync mode: resize immediately
        renderer_->resize(width, height);
    }
}

void VideoRenderController::requestSetColorspace() {
    if (threaded_) {
        colorspace_pending_.store(true);
        notify();
    } else {
        // Sync mode: set immediately
        renderer_->setColorspace();
    }
}

void VideoRenderController::threadFunc() {
    while (running_.load()) {
        // Handle resize first
        if (resize_pending_.exchange(false)) {
            std::lock_guard<std::mutex> lock(resize_mutex_);
            renderer_->resize(resize_width_, resize_height_);
        }

        // Handle colorspace setup
        if (colorspace_pending_.exchange(false)) {
            renderer_->setColorspace();
        }

        // Clear frame notification (we're about to check for frames)
        frame_notified_.store(false);

        // Only render if active and has dimensions
        if (active_.load()) {
            int w = width_.load();
            int h = height_.load();
            if (w > 0 && h > 0 && renderer_->hasFrame()) {
                if (renderer_->render(w, h)) {
                    video_ready_.store(true);
                }
            }
        }

        // Wait for work: frame ready, resize, colorspace, or shutdown
        // 100ms timeout as fallback for shutdown check
        std::unique_lock lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return !running_.load() || resize_pending_.load() ||
                   colorspace_pending_.load() || frame_notified_.load();
        });
    }
}
