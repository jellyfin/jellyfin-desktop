#include "vulkan_subsurface_renderer.h"
#include "mpv/mpv_player.h"
#include <mpv/render.h>
#include <mpv/render_vk.h>
#ifdef __APPLE__
#include "platform/macos_layer.h"
#else
#include "platform/video_surface.h"
#endif

VulkanSubsurfaceRenderer::VulkanSubsurfaceRenderer(MpvPlayer* player, VideoSurface* surface)
    : player_(player), surface_(surface) {}

bool VulkanSubsurfaceRenderer::hasFrame() const {
    return player_->hasFrame();
}

bool VulkanSubsurfaceRenderer::render(int width, int height) {
#if defined(__APPLE__) || defined(_WIN32)
    // FBO mode: we manage the swapchain, mpv renders to our image.
    (void)width; (void)height;
    VkImage image;
    VkImageView view;
    VkFormat format;
    if (surface_->startFrame(&image, &view, &format)) {
        mpv_vulkan_fbo fbo{};
        fbo.image = image;
        fbo.image_view = view;
        fbo.width = surface_->width();
        fbo.height = surface_->height();
        fbo.format = format;
        fbo.current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        fbo.target_layout = surface_->targetImageLayout();

        int flip_y = 0;
        mpv_render_param render_params[] = {
            {MPV_RENDER_PARAM_VULKAN_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };

        mpv_render_context_render(player_->renderContext(), render_params);
        surface_->submitFrame();
        player_->reportSwap();
        return true;
    }
    return false;
#else
    // Swapchain mode: mpv/libplacebo handles frame acquisition and presentation.
    // Pass window size so the swapchain can resize.
    int size[2] = { width, height };
    int flip_y = 0;
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_VULKAN_SWAPCHAIN_SIZE, size},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    mpv_render_context_render(player_->renderContext(), render_params);
    player_->reportSwap();
    return true;
#endif
}

void VulkanSubsurfaceRenderer::setVisible(bool visible) {
    if (visible) {
        surface_->show();
    } else {
        surface_->hide();
    }
}

void VulkanSubsurfaceRenderer::resize(int width, int height) {
#ifdef __APPLE__
    surface_->resize(width, height);
#else
    surface_->recreateSwapchain(width, height);
#endif
}

void VulkanSubsurfaceRenderer::setDestinationSize(int width, int height) {
    surface_->setDestinationSize(width, height);
}

void VulkanSubsurfaceRenderer::setColorspace() {
    surface_->setColorspace();
}

void VulkanSubsurfaceRenderer::cleanup() {
    surface_->cleanup();
}

float VulkanSubsurfaceRenderer::getClearAlpha(bool video_ready) const {
    return video_ready ? 0.0f : 1.0f;
}

bool VulkanSubsurfaceRenderer::isHdr() const {
    return surface_->isHdr();
}

#ifdef __APPLE__
void* VulkanSubsurfaceRenderer::getVideoView() {
    return surface_->getVideoView();
}

void VulkanSubsurfaceRenderer::restoreVideoView() {
    surface_->restoreVideoView();
}
#endif
