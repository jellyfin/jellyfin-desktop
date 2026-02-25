#ifdef _WIN32

#include "dcomp_renderer.h"
#include "mpv/mpv_player_gl.h"
#include "platform/windows_video_layer.h"
#include "context/wgl_context.h"
#include "context/gl_loader.h"
#include "logging.h"

DCompRenderer::DCompRenderer(MpvPlayerGL* player, WindowsVideoLayer* layer)
    : player_(player), layer_(layer) {}

DCompRenderer::~DCompRenderer() {
    cleanup();
}

bool DCompRenderer::initThreaded(WGLContext* wgl) {
    wgl_ = wgl;
    shared_ctx_ = wgl->createSharedContext();
    if (!shared_ctx_) {
        LOG_ERROR(LOG_VIDEO, "[DCompRenderer] Failed to create shared WGL context");
        return false;
    }
    LOG_INFO(LOG_VIDEO, "[DCompRenderer] Initialized for threaded rendering");
    return true;
}

bool DCompRenderer::hasFrame() const {
    return player_->hasFrame();
}

bool DCompRenderer::render(int width, int height) {
    // Make shared context current on render thread
    if (!wgl_->makeCurrent(shared_ctx_)) {
        LOG_ERROR(LOG_VIDEO, "[DCompRenderer] Failed to make shared context current");
        return false;
    }

    // Create FBO on first render (FBOs are per-context, must be created
    // on the render thread's shared context, not the main context)
    if (!layer_->fbo()) {
        if (!layer_->createFBO()) {
            wgl_->makeCurrent(nullptr);
            return false;
        }
    }

    // Lock GL-DXGI interop (also acquires staging_mutex_)
    if (!layer_->lockInterop()) {
        wgl_->makeCurrent(nullptr);
        return false;
    }

    // Render mpv to the interop FBO
    // flip=true: GL FBO is bottom-left origin, D3D swap chain is top-left.
    // CopyResource is a raw copy with no flip, so mpv must render flipped.
    glBindFramebuffer(GL_FRAMEBUFFER, layer_->fbo());
    glViewport(0, 0, layer_->width(), layer_->height());
    player_->render(layer_->width(), layer_->height(), layer_->fbo(), true);

    // Wait for render to complete before releasing interop
    glFinish();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Unlock GL-DXGI interop (releases staging_mutex_)
    layer_->unlockInterop();

    wgl_->makeCurrent(nullptr);

    has_rendered_.store(true);
    frame_ready_.store(true);
    return true;
}

void DCompRenderer::composite(int width, int height) {
    (void)width; (void)height;

    if (frame_ready_.exchange(false)) {
        layer_->present();
        layer_->commit();
    }
}

void DCompRenderer::setVisible(bool visible) {
    if (visible) {
        layer_->show();
    } else {
        layer_->hide();
    }
}

void DCompRenderer::resize(int width, int height) {
    // Called from render thread via VideoRenderController.
    // Need GL context current for destroying FBO and interop GL objects
    // and creating new ones.
    if (!wgl_ || !shared_ctx_) return;

    // Prevent main thread from presenting stale/destroyed resources
    frame_ready_.store(false);

    wgl_->makeCurrent(shared_ctx_);

    // Destroy FBO (per-context, must be done on this context)
    layer_->destroyFBO();

    // Recreate swap chain + staging texture at new size
    // (also destroys/recreates interop registration, holds staging_mutex_)
    layer_->recreateSwapchain(width, height);

    // Re-initialize GL-DXGI interop with the new staging texture
    layer_->initInterop(wgl_);

    // Recreate FBO on this context with the new texture
    layer_->createFBO();

    wgl_->makeCurrent(nullptr);
}

void DCompRenderer::cleanup() {
    if (shared_ctx_ && wgl_) {
        wgl_->makeCurrent(shared_ctx_);

        // Destroy FBO while its owning context is current
        layer_->destroyFBO();

        // Destroy interop GL objects while a context is current
        layer_->destroyInterop();

        wgl_->makeCurrent(nullptr);
        wgl_->destroyContext(shared_ctx_);
        shared_ctx_ = nullptr;
    }

    has_rendered_.store(false);
    frame_ready_.store(false);
}

float DCompRenderer::getClearAlpha(bool video_ready) const {
    // When video is ready, clear with alpha=0 so the DComp video layer
    // shows through the transparent GL window via DWM transparency
    return video_ready ? 0.0f : 1.0f;
}

#endif // _WIN32
