#pragma once

#include "player/media_session.h"
#include <systemd/sd-bus.h>

class MprisBackend : public MediaSessionBackend {
public:
    MprisBackend(MediaSession* session, const std::string& service_suffix = "");
    ~MprisBackend() override;

    void setMetadata(const MediaMetadata& meta) override;
    void setArtwork(const std::string& dataUri) override;
    void setPlaybackState(PlaybackState state) override;
    void setPosition(int64_t position_us) override;
    void setVolume(double volume) override;
    void setCanGoNext(bool can) override;
    void setCanGoPrevious(bool can) override;
    void setRate(double rate) override;
    void setBuffering(bool buffering) override;
    void emitSeeking() override;                     // Lock rate at 0x during seek
    void emitSeeked(int64_t position_us) override;  // Emit Seeked signal when user seeks
    void update() override;
    int getFd() override;

    // Property getters (called from D-Bus vtable)
    const char* getPlaybackStatus() const;
    int64_t getPosition() const { return position_us_; }
    double getVolume() const { return volume_; }
    double getRate() const { return rate_; }
    bool canGoNext() const { return can_go_next_; }
    bool canGoPrevious() const { return can_go_previous_; }
    const MediaMetadata& getMetadata() const { return metadata_; }
    MediaSession* session() { return session_; }

private:
    void emitPropertiesChanged(const char* interface, const char* property);
    void syncRate();  // Apply pending_rate_ or 0 based on seeking_/buffering_

    MediaSession* session_;
    std::string service_name_;
    sd_bus* bus_ = nullptr;
    sd_bus_slot* slot_root_ = nullptr;
    sd_bus_slot* slot_player_ = nullptr;

    MediaMetadata metadata_;
    PlaybackState state_ = PlaybackState::Stopped;
    int64_t position_us_ = 0;
    double volume_ = 1.0;
    double rate_ = 1.0;
    double pending_rate_ = 1.0;  // Stored rate while locked at 0x
    bool seeking_ = false;       // Rate locked by seeking
    bool buffering_ = false;     // Rate locked by buffering
    bool can_go_next_ = false;
    bool can_go_previous_ = false;
};

std::unique_ptr<MediaSessionBackend> createMprisBackend(MediaSession* session, const std::string& service_suffix = "");
