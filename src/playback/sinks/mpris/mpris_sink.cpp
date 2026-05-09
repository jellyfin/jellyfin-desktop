#include "mpris_sink.h"

#include "../../../common.h"
#include "../../../browser/browsers.h"
#include "../../../browser/web_browser.h"
#include "../../../logging.h"

#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <vector>

// D-Bus object path
static const char* MPRIS_PATH = "/org/mpris/MediaPlayer2";
static const char* MPRIS_ROOT_IFACE = "org.mpris.MediaPlayer2";
static const char* MPRIS_PLAYER_IFACE = "org.mpris.MediaPlayer2.Player";
static const char* BASE_SERVICE_NAME = "org.mpris.MediaPlayer2.JellyfinDesktop";

// Root interface property getters
static int prop_get_identity(sd_bus*, const char*, const char*, const char*,
                             sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "s", "Jellyfin Desktop");
}

static int prop_get_can_quit(sd_bus*, const char*, const char*, const char*,
                             sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "b", false);
}

static int prop_get_can_raise(sd_bus*, const char*, const char*, const char*,
                              sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "b", true);
}

static int prop_get_can_set_fullscreen(sd_bus*, const char*, const char*, const char*,
                                       sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "b", true);
}

static int prop_get_fullscreen(sd_bus*, const char*, const char*, const char*,
                               sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "b", false);
}

static int prop_get_has_track_list(sd_bus*, const char*, const char*, const char*,
                                   sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "b", false);
}

static int prop_get_supported_uri_schemes(sd_bus*, const char*, const char*, const char*,
                                          sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "as", 0);
}

static int prop_get_supported_mime_types(sd_bus*, const char*, const char*, const char*,
                                         sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "as", 0);
}

// Root interface methods
static int method_raise(sd_bus_message* m, void* /*userdata*/, sd_bus_error*) {
    // No raise impl wired (was always null-checked); reply ok.
    return sd_bus_reply_method_return(m, "");
}

static int method_quit(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable root_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Identity", "s", prop_get_identity, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanQuit", "b", prop_get_can_quit, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanRaise", "b", prop_get_can_raise, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanSetFullscreen", "b", prop_get_can_set_fullscreen, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Fullscreen", "b", prop_get_fullscreen, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("HasTrackList", "b", prop_get_has_track_list, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedUriSchemes", "as", prop_get_supported_uri_schemes, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedMimeTypes", "as", prop_get_supported_mime_types, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Raise", "", "", method_raise, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Quit", "", "", method_quit, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

// Player interface property getters
static int prop_get_playback_status(sd_bus*, const char*, const char*, const char*,
                                    sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    return sd_bus_message_append(reply, "s", sink->getPlaybackStatus());
}

static int prop_get_position(sd_bus*, const char*, const char*, const char*,
                             sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    return sd_bus_message_append(reply, "x", sink->getPosition());
}

static int prop_get_volume(sd_bus*, const char*, const char*, const char*,
                           sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    return sd_bus_message_append(reply, "d", sink->getVolume());
}

static int prop_get_rate(sd_bus*, const char*, const char*, const char*,
                         sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    return sd_bus_message_append(reply, "d", sink->getRate());
}

static int prop_set_rate(sd_bus*, const char*, const char*, const char*,
                         sd_bus_message* value, void* /*userdata*/, sd_bus_error*) {
    double rate;
    int r = sd_bus_message_read(value, "d", &rate);
    if (r < 0) return r;

    if (rate < 0.25) rate = 0.25;
    if (rate > 2.0) rate = 2.0;

    g_mpv.SetSpeed(rate);
    return 0;
}

static int prop_get_min_rate(sd_bus*, const char*, const char*, const char*,
                             sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "d", 0.25);
}

static int prop_get_max_rate(sd_bus*, const char*, const char*, const char*,
                             sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "d", 2.0);
}

static int prop_get_can_go_next(sd_bus*, const char*, const char*, const char*,
                                sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    return sd_bus_message_append(reply, "b", sink->canGoNext());
}

static int prop_get_can_go_previous(sd_bus*, const char*, const char*, const char*,
                                    sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    return sd_bus_message_append(reply, "b", sink->canGoPrevious());
}

static int prop_get_can_play(sd_bus*, const char*, const char*, const char*,
                             sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    return sd_bus_message_append(reply, "b", sink->canPlay());
}

static int prop_get_can_pause(sd_bus*, const char*, const char*, const char*,
                              sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    return sd_bus_message_append(reply, "b", sink->canPause());
}

static int prop_get_can_seek(sd_bus*, const char*, const char*, const char*,
                             sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    return sd_bus_message_append(reply, "b", sink->canSeek());
}

static int prop_get_can_control(sd_bus*, const char*, const char*, const char*,
                                sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    return sd_bus_message_append(reply, "b", sink->canControl());
}

static int prop_get_metadata(sd_bus*, const char*, const char*, const char*,
                             sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    const auto& meta = sink->getMetadata();

    sd_bus_message_open_container(reply, 'a', "{sv}");

    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append(reply, "s", "mpris:trackid");
    sd_bus_message_open_container(reply, 'v', "o");
    sd_bus_message_append(reply, "o", "/org/jellyfin/track/1");
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    if (meta.duration_us > 0) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "mpris:length");
        sd_bus_message_open_container(reply, 'v', "x");
        sd_bus_message_append(reply, "x", meta.duration_us);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    if (!meta.title.empty()) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "xesam:title");
        sd_bus_message_open_container(reply, 'v', "s");
        sd_bus_message_append(reply, "s", meta.title.c_str());
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    if (!meta.artist.empty()) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "xesam:artist");
        sd_bus_message_open_container(reply, 'v', "as");
        sd_bus_message_append(reply, "as", 1, meta.artist.c_str());
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    if (!meta.album.empty()) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "xesam:album");
        sd_bus_message_open_container(reply, 'v', "s");
        sd_bus_message_append(reply, "s", meta.album.c_str());
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    if (meta.track_number > 0) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "xesam:trackNumber");
        sd_bus_message_open_container(reply, 'v', "i");
        sd_bus_message_append(reply, "i", meta.track_number);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    if (!meta.art_data_uri.empty()) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "mpris:artUrl");
        sd_bus_message_open_container(reply, 'v', "s");
        sd_bus_message_append(reply, "s", meta.art_data_uri.c_str());
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    sd_bus_message_close_container(reply);
    return 0;
}

// Player interface methods (inbound transport from MPRIS clients).
// Callbacks are wired directly into g_mpv / g_web_browser; no transport
// callback indirection.
static int method_play(sd_bus_message* m, void*, sd_bus_error*) {
    g_mpv.Play();
    return sd_bus_reply_method_return(m, "");
}

static int method_pause(sd_bus_message* m, void*, sd_bus_error*) {
    g_mpv.Pause();
    return sd_bus_reply_method_return(m, "");
}

static int method_play_pause(sd_bus_message* m, void*, sd_bus_error*) {
    g_mpv.TogglePause();
    return sd_bus_reply_method_return(m, "");
}

static int method_stop(sd_bus_message* m, void*, sd_bus_error*) {
    g_mpv.Stop();
    return sd_bus_reply_method_return(m, "");
}

static int method_next(sd_bus_message* m, void*, sd_bus_error*) {
    if (g_web_browser)
        g_web_browser->execJs("if(window._nativeHostInput) window._nativeHostInput(['next']);");
    return sd_bus_reply_method_return(m, "");
}

static int method_previous(sd_bus_message* m, void*, sd_bus_error*) {
    if (g_web_browser)
        g_web_browser->execJs("if(window._nativeHostInput) window._nativeHostInput(['previous']);");
    return sd_bus_reply_method_return(m, "");
}

static int method_seek(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* sink = static_cast<MprisSink*>(userdata);
    int64_t offset;
    sd_bus_message_read(m, "x", &offset);
    int64_t new_pos = sink->getPosition() + offset;
    if (new_pos < 0) new_pos = 0;
    int ms = static_cast<int>(new_pos / 1000);
    if (g_web_browser)
        g_web_browser->execJs("if(window._nativeSeek) window._nativeSeek(" + std::to_string(ms) + ");");
    return sd_bus_reply_method_return(m, "");
}

static int method_set_position(sd_bus_message* m, void*, sd_bus_error*) {
    const char* track_id;
    int64_t position;
    sd_bus_message_read(m, "ox", &track_id, &position);
    int ms = static_cast<int>(position / 1000);
    if (g_web_browser)
        g_web_browser->execJs("if(window._nativeSeek) window._nativeSeek(" + std::to_string(ms) + ");");
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable player_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("PlaybackStatus", "s", prop_get_playback_status, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("Rate", "d", prop_get_rate, prop_set_rate, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("MinimumRate", "d", prop_get_min_rate, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MaximumRate", "d", prop_get_max_rate, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Metadata", "a{sv}", prop_get_metadata, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Volume", "d", prop_get_volume, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Position", "x", prop_get_position, 0, 0),
    SD_BUS_PROPERTY("CanGoNext", "b", prop_get_can_go_next, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanGoPrevious", "b", prop_get_can_go_previous, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanPlay", "b", prop_get_can_play, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanPause", "b", prop_get_can_pause, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanSeek", "b", prop_get_can_seek, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanControl", "b", prop_get_can_control, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("Play", "", "", method_play, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Pause", "", "", method_pause, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PlayPause", "", "", method_play_pause, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Stop", "", "", method_stop, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Next", "", "", method_next, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Previous", "", "", method_previous, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Seek", "x", "", method_seek, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetPosition", "ox", "", method_set_position, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

MprisSink::MprisSink(std::string service_suffix)
    : service_suffix_(std::move(service_suffix))
    , service_name_(std::string(BASE_SERVICE_NAME) + service_suffix_) {}

MprisSink::~MprisSink() {
    stop();
}

void MprisSink::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&MprisSink::threadFunc, this);
}

void MprisSink::stop() {
    if (!running_.exchange(false)) return;
    wake().signal();
    if (thread_.joinable()) thread_.join();
}

int64_t MprisSink::getPosition() const {
    // last_snap_ is updated on the sink's own thread alongside D-Bus
    // dispatch, so getter and writer share a thread — no lock needed.
    return last_snap_.position_us;
}

void MprisSink::initBus() {
    int r = sd_bus_open_user(&bus_);
    if (r < 0) {
        LOG_ERROR(LOG_MEDIA, "MPRIS: Failed to connect to session bus: {}", strerror(-r));
        return;
    }

    r = sd_bus_request_name(bus_, service_name_.c_str(), 0);
    if (r < 0) {
        LOG_ERROR(LOG_MEDIA, "MPRIS: Failed to acquire service name: {}", strerror(-r));
        sd_bus_unref(bus_);
        bus_ = nullptr;
        return;
    }

    LOG_INFO(LOG_MEDIA, "MPRIS: Registered as {}", service_name_.c_str());

    r = sd_bus_add_object_vtable(bus_, &slot_root_, MPRIS_PATH,
                                 MPRIS_ROOT_IFACE, root_vtable, this);
    if (r < 0) {
        LOG_ERROR(LOG_MEDIA, "MPRIS: Failed to add root vtable: {}", strerror(-r));
    }

    r = sd_bus_add_object_vtable(bus_, &slot_player_, MPRIS_PATH,
                                 MPRIS_PLAYER_IFACE, player_vtable, this);
    if (r < 0) {
        LOG_ERROR(LOG_MEDIA, "MPRIS: Failed to add player vtable: {}", strerror(-r));
    }
}

void MprisSink::teardownBus() {
    if (slot_player_) { sd_bus_slot_unref(slot_player_); slot_player_ = nullptr; }
    if (slot_root_) { sd_bus_slot_unref(slot_root_); slot_root_ = nullptr; }
    if (bus_) {
        sd_bus_release_name(bus_, service_name_.c_str());
        sd_bus_unref(bus_);
        bus_ = nullptr;
    }
}

void MprisSink::threadFunc() {
    initBus();

    int dbus_fd = bus_ ? sd_bus_get_fd(bus_) : -1;
    int wake_fd = wake().fd();

    while (running_.load(std::memory_order_relaxed)) {
        // Drain queued playback events.
        wake().drain();
        pump();

        // Process pending D-Bus messages until quiescent.
        if (bus_) {
            int r;
            do { r = sd_bus_process(bus_, nullptr); } while (r > 0);
        }

        // Block on either fd becoming ready, with a 100ms cap so a
        // flipped running_ flag is observed promptly.
        struct pollfd fds[2];
        int nfds = 0;
        if (dbus_fd >= 0) { fds[nfds].fd = dbus_fd; fds[nfds].events = POLLIN; nfds++; }
        if (wake_fd >= 0) { fds[nfds].fd = wake_fd; fds[nfds].events = POLLIN; nfds++; }
        if (nfds == 0) break;
        poll(fds, nfds, 100);
    }

    teardownBus();
}

void MprisSink::deliver(const PlaybackEvent& ev) {
    last_snap_ = ev.snapshot;
    switch (ev.kind) {
    case PlaybackEvent::Kind::MetadataChanged:
        // Same-Id dedup: a same-Id setMetadata is a semantic no-op
        // (identical item) and must not churn backend state — otherwise
        // empty art fields in the incoming meta would clobber cached art
        // from notifyArtwork on every variant switch.
        if (!ev.metadata.id.empty() && ev.metadata.id == content_.metadata.id) break;
        content_.metadata = ev.metadata;
        recomputeAndEmit(ev.snapshot);
        break;
    case PlaybackEvent::Kind::ArtworkChanged:
        content_.metadata.art_data_uri = ev.artwork_uri;
        recomputeAndEmit(ev.snapshot);
        break;
    case PlaybackEvent::Kind::QueueCapsChanged:
        content_.can_go_next = ev.can_go_next;
        content_.can_go_previous = ev.can_go_prev;
        recomputeAndEmit(ev.snapshot);
        break;
    case PlaybackEvent::Kind::Started:
        recomputeAndEmit(ev.snapshot);
        // Position re-anchor on resume so MPRIS clients see the correct anchor.
        if (bus_)
            sd_bus_emit_signal(bus_, MPRIS_PATH, MPRIS_PLAYER_IFACE,
                               "Seeked", "x", ev.snapshot.position_us);
        break;
    case PlaybackEvent::Kind::Seeked:
        if (bus_)
            sd_bus_emit_signal(bus_, MPRIS_PATH, MPRIS_PLAYER_IFACE,
                               "Seeked", "x", ev.snapshot.position_us);
        break;
    case PlaybackEvent::Kind::Paused:
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error:
    case PlaybackEvent::Kind::SeekingChanged:
    case PlaybackEvent::Kind::BufferingChanged:
    case PlaybackEvent::Kind::TrackLoaded:
    case PlaybackEvent::Kind::RateChanged:
        recomputeAndEmit(ev.snapshot);
        break;
    case PlaybackEvent::Kind::PositionChanged:
        // MPRIS Position is polled, not signaled. last_snap_ already
        // updated above so getPosition() reads the latest value.
        break;
    case PlaybackEvent::Kind::DurationChanged:
        // Duration ships inside the metadata payload; bare DurationChanged
        // events from mpv aren't surfaced to MPRIS.
        break;
    case PlaybackEvent::Kind::MediaTypeChanged:
    case PlaybackEvent::Kind::FullscreenChanged:
    case PlaybackEvent::Kind::OsdDimsChanged:
    case PlaybackEvent::Kind::BufferedRangesChanged:
    case PlaybackEvent::Kind::DisplayHzChanged:
        break;
    }
}

void MprisSink::recomputeAndEmit(const PlaybackSnapshot& snap) {
    MprisView next = project(snap, content_);
    auto changed = diff(last_, next);
    LOG_DEBUG(LOG_MEDIA,
        "mpris: snap phase={} buffering={} seeking={} -> status={} rate={} canSeek={} dur={} changed={}",
        static_cast<int>(snap.phase), snap.buffering, snap.seeking,
        next.playback_status.c_str(), next.rate, next.can_seek,
        next.metadata.duration_us, changed.size());
    last_ = std::move(next);
    if (!bus_ || changed.empty()) return;
    emitChanged(changed);
}

void MprisSink::emitChanged(const std::vector<const char*>& names) {
    std::vector<char*> argv;
    argv.reserve(names.size() + 1);
    for (const char* n : names) argv.push_back(const_cast<char*>(n));
    argv.push_back(nullptr);
    sd_bus_emit_properties_changed_strv(bus_, MPRIS_PATH, MPRIS_PLAYER_IFACE,
                                        argv.data());
}
