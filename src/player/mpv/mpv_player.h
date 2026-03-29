#pragma once

#include <string>
#include <functional>
#include <utility>
#include <vector>
#include <atomic>

struct mpv_handle;
struct mpv_render_context;
struct mpv_render_param;

class MpvPlayer {
public:
    using RedrawCallback = std::function<void()>;
    using PositionCallback = std::function<void(double ms)>;
    using DurationCallback = std::function<void(double ms)>;
    using StateCallback = std::function<void(bool paused)>;
    using PlaybackCallback = std::function<void()>;
    using SeekCallback = std::function<void(double ms)>;
    using SpeedCallback = std::function<void(double speed)>;
    using BufferingCallback = std::function<void(bool buffering, double ms)>;
    using CoreIdleCallback = std::function<void(bool idle, double ms)>;
    struct BufferedRange { int64_t start; int64_t end; };
    using BufferedRangesCallback = std::function<void(const std::vector<BufferedRange>&)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using WakeupCallback = std::function<void()>;
    using PreInitHook = std::function<void(mpv_handle*)>;

    MpvPlayer();
    ~MpvPlayer();

    // Initialize mpv. preInitHook is called after mpv_create but before mpv_initialize
    // (use it to set HDR options, hwdec-codecs, etc.)
    bool init(const char* hwdec = "auto-safe", const PreInitHook& preInitHook = nullptr);

    // Create render context from caller-built params (Vulkan or OpenGL).
    // Caller builds the mpv_render_param array; this wraps mpv_render_context_create
    // and sets the update callback.
    bool createRenderContext(mpv_render_param* params);

    mpv_render_context* renderContext() const { return render_ctx_; }

    void cleanup();

    // Playback control
    bool loadFile(const std::string& path, double startSeconds = 0.0);
    void stop();
    void pause();
    void play();
    void seek(double seconds);
    void setVolume(int volume);
    void setMuted(bool muted);
    void setSpeed(double speed);
    void setNormalizationGain(double gainDb);
    void setSubtitleTrack(int sid);
    void setAudioTrack(int aid);
    void setAudioDelay(double seconds);

    // State queries
    double getPosition() const;
    double getDuration() const;
    double getSpeed() const;
    bool isPaused() const;
    bool isPlaying() const { return playing_; }
    bool hasFrame() const;
    bool needsRedraw() const { return needs_redraw_.load(); }
    void clearRedrawFlag() { needs_redraw_ = false; }
    void reportSwap();

    // Events
    void processEvents();

    // Callback setters
    void setRedrawCallback(RedrawCallback cb) { redraw_callback_ = std::move(cb); }
    void setPositionCallback(PositionCallback cb) { on_position_ = std::move(cb); }
    void setDurationCallback(DurationCallback cb) { on_duration_ = std::move(cb); }
    void setStateCallback(StateCallback cb) { on_state_ = std::move(cb); }
    void setPlayingCallback(PlaybackCallback cb) { on_playing_ = std::move(cb); }
    void setFinishedCallback(PlaybackCallback cb) { on_finished_ = std::move(cb); }
    void setCanceledCallback(PlaybackCallback cb) { on_canceled_ = std::move(cb); }
    void setSpeedCallback(SpeedCallback cb) { on_speed_ = std::move(cb); }
    void setSeekingCallback(SeekCallback cb) { on_seeking_ = std::move(cb); }
    void setSeekedCallback(SeekCallback cb) { on_seeked_ = std::move(cb); }
    void setBufferingCallback(BufferingCallback cb) { on_buffering_ = std::move(cb); }
    void setCoreIdleCallback(CoreIdleCallback cb) { on_core_idle_ = std::move(cb); }
    void setBufferedRangesCallback(BufferedRangesCallback cb) { on_buffered_ranges_ = std::move(cb); }
    void setErrorCallback(ErrorCallback cb) { on_error_ = std::move(cb); }
    void setWakeupCallback(WakeupCallback cb) { on_wakeup_ = std::move(cb); }

private:
    static void onMpvRedraw(void* ctx);
    static void onMpvWakeup(void* ctx);
    void handleMpvEvent(struct mpv_event* event);

    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ctx_ = nullptr;

    RedrawCallback redraw_callback_;
    PositionCallback on_position_;
    DurationCallback on_duration_;
    StateCallback on_state_;
    PlaybackCallback on_playing_;
    PlaybackCallback on_finished_;
    PlaybackCallback on_canceled_;
    SpeedCallback on_speed_;
    SeekCallback on_seeking_;
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
