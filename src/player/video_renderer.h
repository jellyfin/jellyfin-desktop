#pragma once

class VideoRenderer {
public:
    virtual ~VideoRenderer() = default;

    // Rendering
    virtual bool hasFrame() const = 0;
    virtual bool render(int width, int height) = 0;

    // Composite video to screen (for threaded OpenGL renderers)
    virtual void composite(int width, int height) { (void)width; (void)height; }

    // Subsurface lifecycle (no-op for composite renderers)
    virtual void setVisible(bool visible) = 0;
    virtual void resize(int width, int height) = 0;
    virtual void setDestinationSize(int width, int height) = 0;  // HiDPI logical size
    virtual void setColorspace() = 0;
    virtual void cleanup() = 0;

    // For frame clear decision
    virtual float getClearAlpha(bool video_ready) const = 0;

    // HDR query
    virtual bool isHdr() const = 0;

    // PiP: get the native video view (NSView* on macOS, nullptr otherwise)
    virtual void* getVideoView() { return nullptr; }
};
