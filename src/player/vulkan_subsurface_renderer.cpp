#include "vulkan_subsurface_renderer.h"
#include "mpv/mpv_player_vk.h"
#ifdef __APPLE__
#include "platform/macos_layer.h"
#else
#include "platform/video_surface.h"
#endif

VulkanSubsurfaceRenderer::VulkanSubsurfaceRenderer(MpvPlayerVk* player, VideoSurface* surface)
    : player_(player), surface_(surface) {}

bool VulkanSubsurfaceRenderer::hasFrame() const {
    return player_->hasFrame();
}

bool VulkanSubsurfaceRenderer::render(int width, int height) {
    (void)width; (void)height;  // Uses surface dimensions
    VkImage image;
    VkImageView view;
    VkFormat format;
    if (surface_->startFrame(&image, &view, &format)) {
        player_->render(image, view, surface_->width(), surface_->height(), format);
        surface_->submitFrame();
        player_->reportSwap();
        return true;
    }
    return false;
}

void VulkanSubsurfaceRenderer::setVisible(bool) {}

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
