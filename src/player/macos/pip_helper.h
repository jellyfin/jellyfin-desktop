#pragma once
#ifdef __APPLE__

// macOS native Picture-in-Picture using the private PIPViewController API.
// Reparents the actual video view (CAMetalLayer) into a PiP window.
// mpv continues rendering directly — no frame capture needed.

#include <functional>

class MacOSPiPHelper {
public:
    MacOSPiPHelper();
    ~MacOSPiPHelper();

    static bool isSupported();

    // Start PiP by reparenting the given video NSView into a floating PiP window.
    // videoView: the NSView* containing the CAMetalLayer (cast to void*)
    // videoW/videoH: native video dimensions for aspect ratio
    void start(void* videoView, int videoW, int videoH);
    void stop();
    void toggle(void* videoView, int videoW, int videoH);

    bool isActive() const;

    // Update playback state so PiP buttons reflect correctly
    void setPlaying(bool playing);

    // Called when PiP play/pause button is pressed
    void setPlayPauseCallback(std::function<void(bool playing)> cb);

    // Called when PiP closes — restore the video view to the main window
    void setRestoreCallback(std::function<void()> cb);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

#endif // __APPLE__
