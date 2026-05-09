#pragma once

#include "player/media_session.h"
#include "player/mpris/mpris_projection.h"

#include <systemd/sd-bus.h>

class MprisBackend : public MediaSessionBackend {
public:
    MprisBackend(MediaSession* session, const std::string& service_suffix = "");
    ~MprisBackend() override;

    // All setters write the relevant MprisContent field (or do nothing,
    // for inputs that live in the PlaybackCoordinator snapshot) and
    // call recomputeAndEmit(). No setter names an MPRIS property.
    void setMetadata(const MediaMetadata& meta) override;
    void setArtwork(const std::string& dataUri) override;
    void setPlaybackState(PlaybackState state) override;
    void setPosition(int64_t position_us) override;
    void setVolume(double volume) override;
    void setCanGoNext(bool can) override;
    void setCanGoPrevious(bool can) override;
    void setRate(double rate) override;
    void setBuffering(bool buffering) override;
    void setSeeking(bool seeking) override;
    void emitSeeked(int64_t position_us) override;
    void update() override;
    int getFd() override;

    // Property getters used by the D-Bus vtable. Each reads a single
    // field of last_, so getters never re-derive logic.
    const char* getPlaybackStatus() const { return last_.playback_status.c_str(); }
    int64_t getPosition() const;
    double getVolume() const { return last_.volume; }
    double getRate() const { return last_.rate; }
    bool canGoNext() const { return last_.can_go_next; }
    bool canGoPrevious() const { return last_.can_go_previous; }
    bool canPlay() const { return last_.can_play; }
    bool canPause() const { return last_.can_pause; }
    bool canSeek() const { return last_.can_seek; }
    bool canControl() const { return last_.can_control; }
    const MediaMetadata& getMetadata() const { return last_.metadata; }
    MediaSession* session() { return session_; }

private:
    void recomputeAndEmit();
    void emitChanged(const std::vector<const char*>& names);

    MediaSession* session_;
    std::string service_name_;
    sd_bus* bus_ = nullptr;
    sd_bus_slot* slot_root_ = nullptr;
    sd_bus_slot* slot_player_ = nullptr;

    MprisContent content_;
    MprisView last_;          // mirrors what bus clients last saw
};

std::unique_ptr<MediaSessionBackend> createMprisBackend(MediaSession* session, const std::string& service_suffix = "");
