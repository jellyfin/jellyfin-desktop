#pragma once

#include <string>
#include <functional>
#include <vector>

// Abstract base class for mpv player implementations
class MpvPlayer {
public:
    using RedrawCallback = std::function<void()>;
    using PositionCallback = std::function<void(double ms)>;
    using DurationCallback = std::function<void(double ms)>;
    using StateCallback = std::function<void(bool paused)>;
    using PlaybackCallback = std::function<void()>;
    using SeekCallback = std::function<void(double ms)>;
    using BufferingCallback = std::function<void(bool buffering, double ms)>;
    using CoreIdleCallback = std::function<void(bool idle, double ms)>;
    struct BufferedRange { int64_t start; int64_t end; };
    using BufferedRangesCallback = std::function<void(const std::vector<BufferedRange>&)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using WakeupCallback = std::function<void()>;

    virtual ~MpvPlayer() = default;

    // Playback control
    virtual bool loadFile(const std::string& path, double startSeconds = 0.0) = 0;
    virtual void stop() = 0;
    virtual void pause() = 0;
    virtual void play() = 0;
    virtual void seek(double seconds) = 0;
    virtual void setVolume(int volume) = 0;
    virtual void setMuted(bool muted) = 0;
    virtual void setSpeed(double speed) = 0;
    virtual void setNormalizationGain(double gainDb) = 0;
    virtual void setSubtitleTrack(int sid) = 0;
    virtual void setAudioTrack(int aid) = 0;
    virtual void setAudioDelay(double seconds) = 0;

    // State queries
    virtual double getPosition() const = 0;
    virtual double getDuration() const = 0;
    virtual double getSpeed() const = 0;
    virtual bool isPaused() const = 0;
    virtual bool isPlaying() const = 0;
    virtual bool hasFrame() const = 0;
    virtual bool isHdr() const = 0;
    virtual bool needsRedraw() const = 0;
    virtual void clearRedrawFlag() = 0;
    virtual void reportSwap() = 0;

    // Events
    virtual void processEvents() = 0;
    virtual void cleanup() = 0;

    // Callback setters
    virtual void setRedrawCallback(RedrawCallback cb) = 0;
    virtual void setPositionCallback(PositionCallback cb) = 0;
    virtual void setDurationCallback(DurationCallback cb) = 0;
    virtual void setStateCallback(StateCallback cb) = 0;
    virtual void setPlayingCallback(PlaybackCallback cb) = 0;
    virtual void setFinishedCallback(PlaybackCallback cb) = 0;
    virtual void setCanceledCallback(PlaybackCallback cb) = 0;
    virtual void setSeekedCallback(SeekCallback cb) = 0;
    virtual void setBufferingCallback(BufferingCallback cb) = 0;
    virtual void setCoreIdleCallback(CoreIdleCallback cb) = 0;
    virtual void setBufferedRangesCallback(BufferedRangesCallback cb) = 0;
    virtual void setErrorCallback(ErrorCallback cb) = 0;
    virtual void setWakeupCallback(WakeupCallback cb) = 0;
};
