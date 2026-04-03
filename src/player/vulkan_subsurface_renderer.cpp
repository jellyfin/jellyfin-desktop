#include "vulkan_subsurface_renderer.h"
#include "mpv/mpv_player.h"
#include <mpv/render.h>
#include <mpv/render_vk.h>
#ifdef __APPLE__
#include "platform/macos_layer.h"
#elif defined(_WIN32)
#include "platform/windows_video_layer.h"
#else
#include "platform/video_surface.h"
#include "platform/wayland_subsurface.h"
#endif

VulkanSubsurfaceRenderer::VulkanSubsurfaceRenderer(MpvPlayer* player, VideoSurface* surface)
    : player_(player), surface_(surface) {}

bool VulkanSubsurfaceRenderer::hasFrame() const {
    return player_->hasFrame();
}

bool VulkanSubsurfaceRenderer::render(int width, int height) {
#if !defined(__APPLE__) && !defined(_WIN32)
    auto* wl = static_cast<WaylandSubsurface*>(surface_);

    // Drain any pending color management events from the compositor.
    if (wl)
        wl->dispatchColorEvents();

    // Dmabuf path: render to an offscreen VkImage, present via wl_surface_attach.
    // No Vulkan swapchain — we own the surface for HDR color management.
    if (wl && wl->hasDmabufPool()) {
        // Resize pool if actual buffer dimensions don't match render target.
        // Compare against dmabuf pool dimensions, not swapchain_extent_ —
        // recreateSwapchain updates swapchain_extent_ without touching the
        // pool, so checking wl->width() would miss the mismatch.
        if (static_cast<int>(wl->dmabufWidth()) != width || static_cast<int>(wl->dmabufHeight()) != height) {
            wl->initDmabufPool(width, height);
        }

        auto* buf = wl->acquireBuffer();
        if (!buf) return false;  // all buffers busy

        mpv_vulkan_fbo fbo{};
        fbo.image = buf->image;
        fbo.image_view = buf->view;
        fbo.width = width;
        fbo.height = height;
        fbo.format = wl->dmabufFormat();
        fbo.current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        fbo.target_layout = VK_IMAGE_LAYOUT_GENERAL;

        int flip_y = 0;
        mpv_render_param render_params[] = {
            {MPV_RENDER_PARAM_VULKAN_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };

        mpv_render_context_render(player_->renderContext(), render_params);

        // After render, video.c writes content_peak with the source's
        // mastering peak. Update the surface image description so the
        // compositor knows the actual content range — matches standalone
        // mpv's vo_wayland_handle_color → set_color_management.
        wl->updateContentPeak();

        wl->presentBuffer(buf);
        player_->reportSwap();
        return true;
    }
#endif

    // Swapchain mode: mpv/libplacebo handles frame acquisition and presentation.
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
}

void VulkanSubsurfaceRenderer::setVisible(bool visible) {
    if (visible) {
        surface_->show();
    } else {
        surface_->hide();
    }
}

void VulkanSubsurfaceRenderer::resize(int width, int height) {
#if defined(__APPLE__) || defined(_WIN32)
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
