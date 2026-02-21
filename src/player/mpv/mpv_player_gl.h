#pragma once

#include "mpv_player.h"
#ifdef _WIN32
#include "context/wgl_context.h"
using GLContext = WGLContext;
#elif defined(__APPLE__)
#include "context/cgl_context.h"
using GLContext = CGLContext;
#else
#include "context/egl_context.h"
using GLContext = EGLContext_;
#endif
#include <atomic>

struct mpv_handle;
struct mpv_render_context;

class MpvPlayerGL : public MpvPlayer {
public:

    MpvPlayerGL();
    ~MpvPlayerGL() override;

    bool init(GLContext* gl, const char* hwdec = "auto-safe");
    void cleanup() override;
    bool loadFile(const std::string& path, double startSeconds = 0.0) override;

    void processEvents() override;
    bool hasFrame() const override;

    // Render to the default framebuffer (or specified FBO)
    // flip: true to flip Y (required for EGL/WGL coordinate systems)
    void render(int width, int height, int fbo = 0, bool flip = true);

    // Playback control
    void stop() override;
    void pause() override;
    void play() override;
    void seek(double seconds) override;
    void setVolume(int volume) override;
    void setMuted(bool muted) override;
    void setSpeed(double speed) override;
    void setNormalizationGain(double gainDb) override;
    void setSubtitleTrack(int sid) override;
    void setAudioTrack(int aid) override;
    void setAudioDelay(double seconds) override;

    // State queries
    double getPosition() const override;
    double getDuration() const override;
    double getSpeed() const override;
    bool isPaused() const override;
    bool isPlaying() const override { return playing_; }
    bool needsRedraw() const override { return needs_redraw_.load(); }
    void clearRedrawFlag() override { needs_redraw_ = false; }

    void setRedrawCallback(RedrawCallback cb) override { redraw_callback_ = cb; }

    void setPositionCallback(PositionCallback cb) override { on_position_ = cb; }
    void setDurationCallback(DurationCallback cb) override { on_duration_ = cb; }
    void setStateCallback(StateCallback cb) override { on_state_ = cb; }
    void setPlayingCallback(PlaybackCallback cb) override { on_playing_ = cb; }
    void setFinishedCallback(PlaybackCallback cb) override { on_finished_ = cb; }
    void setCanceledCallback(PlaybackCallback cb) override { on_canceled_ = cb; }
    void setSeekedCallback(SeekCallback cb) override { on_seeked_ = cb; }
    void setBufferingCallback(BufferingCallback cb) override { on_buffering_ = cb; }
    void setCoreIdleCallback(CoreIdleCallback cb) override { on_core_idle_ = cb; }
    void setBufferedRangesCallback(BufferedRangesCallback cb) override { on_buffered_ranges_ = cb; }
    void setErrorCallback(ErrorCallback cb) override { on_error_ = cb; }
    void setWakeupCallback(WakeupCallback cb) override { on_wakeup_ = cb; }

    bool isHdr() const override { return false; }  // OpenGL path doesn't support HDR

private:
    static void onMpvRedraw(void* ctx);
    static void onMpvWakeup(void* ctx);
    void handleMpvEvent(struct mpv_event* event);

    GLContext* gl_ = nullptr;
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ctx_ = nullptr;

    RedrawCallback redraw_callback_;
    PositionCallback on_position_;
    DurationCallback on_duration_;
    StateCallback on_state_;
    PlaybackCallback on_playing_;
    PlaybackCallback on_finished_;
    PlaybackCallback on_canceled_;
    SeekCallback on_seeked_;
    BufferingCallback on_buffering_;
    CoreIdleCallback on_core_idle_;
    BufferedRangesCallback on_buffered_ranges_;
    ErrorCallback on_error_;
    WakeupCallback on_wakeup_;

    std::atomic<bool> needs_redraw_{false};
    std::atomic<bool> has_events_{false};
    bool playing_ = false;
    bool seeking_ = false;
    double last_position_ = 0.0;
};
