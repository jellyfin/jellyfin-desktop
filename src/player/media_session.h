#pragma once

#include <string>
#include <functional>
#include <cstdint>
#include <memory>
#include <vector>

enum class MediaType { Unknown, Audio, Video };

struct MediaMetadata {
    std::string id;            // Jellyfin item Id; identity across bitrate / variant switches
    std::string title;
    std::string artist;
    std::string album;
    int track_number = 0;
    int64_t duration_us = 0;
    std::string art_url;       // Jellyfin URL
    std::string art_data_uri;  // base64 data URI after fetch
    MediaType media_type = MediaType::Unknown;

    bool operator==(const MediaMetadata& o) const {
        return id == o.id && title == o.title && artist == o.artist && album == o.album
            && track_number == o.track_number && duration_us == o.duration_us
            && art_url == o.art_url && art_data_uri == o.art_data_uri
            && media_type == o.media_type;
    }
};

enum class PlaybackState { Stopped, Playing, Paused };

class MediaSessionBackend {
public:
    virtual ~MediaSessionBackend() = default;
    virtual void setMetadata(const MediaMetadata& meta) = 0;
    virtual void setArtwork(const std::string& dataUri) = 0;  // Update artwork separately
    virtual void setPlaybackState(PlaybackState state) = 0;
    virtual void setPosition(int64_t position_us) = 0;
    virtual void setVolume(double volume) = 0;
    virtual void setCanGoNext(bool can) = 0;
    virtual void setCanGoPrevious(bool can) = 0;
    virtual void setRate(double rate) = 0;
    virtual void setBuffering(bool /*buffering*/) {}
    virtual void setSeeking(bool /*seeking*/) {}
    virtual void emitSeeked(int64_t /*position_us*/) {}
    virtual void update() = 0;  // Called from event loop to process events
    virtual int getFd() = 0;    // File descriptor for poll, -1 if none
};

class MediaSession {
public:
    // Creates a MediaSession with the platform backend and transport callbacks wired up.
    static std::unique_ptr<MediaSession> create();

    MediaSession();
    ~MediaSession();

    void setMetadata(const MediaMetadata& meta);
    void setArtwork(const std::string& dataUri);  // Update artwork separately (async fetch)
    void setPlaybackState(PlaybackState state);
    void setPosition(int64_t position_us);
    void setVolume(double volume);
    void setCanGoNext(bool can);
    void setCanGoPrevious(bool can);
    void setRate(double rate);
    void setBuffering(bool buffering);
    void setSeeking(bool seeking);
    void emitSeeked(int64_t position_us);

    // Called from event loop
    void update();
    int getFd();  // File descriptor for poll, -1 if none

    // Wire all transport callbacks to mpv/JS. Call after adding backends.
    void wireTransportCallbacks();

    // Control callbacks
    std::function<void()> onPlay;
    std::function<void()> onPause;
    std::function<void()> onPlayPause;
    std::function<void()> onStop;
    std::function<void(int64_t)> onSeek;  // position in microseconds
    std::function<void()> onNext;
    std::function<void()> onPrevious;
    std::function<void()> onRaise;
    std::function<void(bool)> onSetFullscreen;
    std::function<void(double)> onSetRate;

    // Backend management
    void addBackend(std::unique_ptr<MediaSessionBackend> backend) { backends_.push_back(std::move(backend)); }

private:
    std::vector<std::unique_ptr<MediaSessionBackend>> backends_;
    PlaybackState state_ = PlaybackState::Stopped;
    // Last metadata fan-out's content. Used to dedup setMetadata calls
    // by Jellyfin item Id: a same-Id setMetadata is a semantic no-op
    // (identical item) and must not churn backend state — otherwise
    // empty art fields in the incoming meta would clobber cached art
    // from notifyArtwork on every variant switch (bitrate / transcode
    // audio change).
    MediaMetadata last_metadata_;
};
