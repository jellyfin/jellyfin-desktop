#pragma once

#include "../queued_sink.h"
#include "mpris_projection.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <systemd/sd-bus.h>

// MPRIS direct sink. Owns its own thread that runs an sd-bus event loop
// (poll on D-Bus fd + queued_sink wake fd). Drains queued PlaybackEvents
// via QueuedPlaybackSink::pump() in the same loop, translating each event
// into MprisContent updates + recompute + property-changed emit.
//
// Inbound transport callbacks (Play/Pause/Next/Previous/Seek/SetRate from
// MPRIS clients) are wired into mpv / web_browser directly inside the
// vtable handlers — no intermediate transport-callback object.
class MprisSink final : public QueuedPlaybackSink {
public:
    explicit MprisSink(std::string service_suffix = "");
    ~MprisSink();

    void start();
    void stop();

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

protected:
    void deliver(const PlaybackEvent& ev) override;

private:
    void threadFunc();
    void initBus();
    void teardownBus();
    void recomputeAndEmit(const PlaybackSnapshot& snap);
    void emitChanged(const std::vector<const char*>& names);

    std::string service_suffix_;
    std::string service_name_;
    sd_bus* bus_ = nullptr;
    sd_bus_slot* slot_root_ = nullptr;
    sd_bus_slot* slot_player_ = nullptr;

    MprisContent content_;
    MprisView last_;
    PlaybackSnapshot last_snap_;

    std::thread thread_;
    std::atomic<bool> running_{false};
};
