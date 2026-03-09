#pragma once
#ifdef _WIN32

#include "video_renderer.h"
#include <atomic>
#include <windows.h>

class MpvPlayer;
class WindowsVideoLayer;
class WGLContext;

class DCompRenderer : public VideoRenderer {
public:
    DCompRenderer(MpvPlayer* player, WindowsVideoLayer* layer);
    ~DCompRenderer();

    bool initThreaded(WGLContext* wgl);

    // VideoRenderer interface
    bool hasFrame() const override;
    bool render(int width, int height) override;      // Render thread
    void composite(int width, int height) override;    // Main thread -> DComp present
    void setVisible(bool visible) override;
    void resize(int width, int height) override;
    void setDestinationSize(int, int) override {}
    void setColorspace() override {}
    void cleanup() override;
    float getClearAlpha(bool video_ready) const override;
    bool isHdr() const override { return false; }

private:
    MpvPlayer* player_;
    WindowsVideoLayer* layer_;
    WGLContext* wgl_ = nullptr;
    HGLRC shared_ctx_ = nullptr;
    std::atomic<bool> has_rendered_{false};
    std::atomic<bool> frame_ready_{false};
};

#endif // _WIN32
