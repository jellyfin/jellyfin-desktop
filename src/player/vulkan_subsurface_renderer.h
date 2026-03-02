#pragma once
#include "video_renderer.h"

class MpvPlayerVk;

// VideoSurface is a type alias on macOS/Windows, forward-declare the actual class
#ifdef __APPLE__
class MacOSVideoLayer;
using VideoSurface = MacOSVideoLayer;
#else
class VideoSurface;
#endif

class VulkanSubsurfaceRenderer : public VideoRenderer {
public:
    VulkanSubsurfaceRenderer(MpvPlayerVk* player, VideoSurface* surface);
    bool hasFrame() const override;
    bool render(int width, int height) override;
    void setVisible(bool visible) override;
    void resize(int width, int height) override;
    void setDestinationSize(int width, int height) override;
    void setColorspace() override;
    void cleanup() override;
    float getClearAlpha(bool video_ready) const override;
    bool isHdr() const override;
private:
    MpvPlayerVk* player_;
    VideoSurface* surface_;
};
