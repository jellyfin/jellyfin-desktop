#include "player/mpris/media_session_mpris.h"
#include <cstring>
#include <string>
#include "common.h"
#include "logging.h"
#include "playback/coordinator.h"

// D-Bus object path
static const char* MPRIS_PATH = "/org/mpris/MediaPlayer2";
static const char* MPRIS_ROOT_IFACE = "org.mpris.MediaPlayer2";
static const char* MPRIS_PLAYER_IFACE = "org.mpris.MediaPlayer2.Player";
static const char* BASE_SERVICE_NAME = "org.mpris.MediaPlayer2.JellyfinDesktop";

// Root interface property getters
static int prop_get_identity(sd_bus* bus, const char* path, const char* interface,
                             const char* property, sd_bus_message* reply,
                             void* userdata, sd_bus_error* error) {
    return sd_bus_message_append(reply, "s", "Jellyfin Desktop");
}

static int prop_get_can_quit(sd_bus* bus, const char* path, const char* interface,
                             const char* property, sd_bus_message* reply,
                             void* userdata, sd_bus_error* error) {
    return sd_bus_message_append(reply, "b", false);
}

static int prop_get_can_raise(sd_bus* bus, const char* path, const char* interface,
                              const char* property, sd_bus_message* reply,
                              void* userdata, sd_bus_error* error) {
    return sd_bus_message_append(reply, "b", true);
}

static int prop_get_can_set_fullscreen(sd_bus* bus, const char* path, const char* interface,
                                       const char* property, sd_bus_message* reply,
                                       void* userdata, sd_bus_error* error) {
    return sd_bus_message_append(reply, "b", true);
}

static int prop_get_fullscreen(sd_bus* bus, const char* path, const char* interface,
                               const char* property, sd_bus_message* reply,
                               void* userdata, sd_bus_error* error) {
    // TODO: track actual fullscreen state
    return sd_bus_message_append(reply, "b", false);
}

static int prop_get_has_track_list(sd_bus* bus, const char* path, const char* interface,
                                   const char* property, sd_bus_message* reply,
                                   void* userdata, sd_bus_error* error) {
    return sd_bus_message_append(reply, "b", false);
}

static int prop_get_supported_uri_schemes(sd_bus* bus, const char* path, const char* interface,
                                          const char* property, sd_bus_message* reply,
                                          void* userdata, sd_bus_error* error) {
    return sd_bus_message_append(reply, "as", 0);
}

static int prop_get_supported_mime_types(sd_bus* bus, const char* path, const char* interface,
                                         const char* property, sd_bus_message* reply,
                                         void* userdata, sd_bus_error* error) {
    return sd_bus_message_append(reply, "as", 0);
}

// Root interface methods
static int method_raise(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    auto* session = backend->session();
    if (session->onRaise) session->onRaise();
    return sd_bus_reply_method_return(m, "");
}

static int method_quit(sd_bus_message* m, void* userdata, sd_bus_error* error) {
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
static int prop_get_playback_status(sd_bus* bus, const char* path, const char* interface,
                                    const char* property, sd_bus_message* reply,
                                    void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    return sd_bus_message_append(reply, "s", backend->getPlaybackStatus());
}

static int prop_get_position(sd_bus* bus, const char* path, const char* interface,
                             const char* property, sd_bus_message* reply,
                             void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    return sd_bus_message_append(reply, "x", backend->getPosition());
}

static int prop_get_volume(sd_bus* bus, const char* path, const char* interface,
                           const char* property, sd_bus_message* reply,
                           void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    return sd_bus_message_append(reply, "d", backend->getVolume());
}

static int prop_get_rate(sd_bus* bus, const char* path, const char* interface,
                         const char* property, sd_bus_message* reply,
                         void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    return sd_bus_message_append(reply, "d", backend->getRate());
}

static int prop_set_rate(sd_bus* bus, const char* path, const char* interface,
                         const char* property, sd_bus_message* value,
                         void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    double rate;
    int r = sd_bus_message_read(value, "d", &rate);
    if (r < 0) return r;

    // Clamp to valid range
    if (rate < 0.25) rate = 0.25;
    if (rate > 2.0) rate = 2.0;

    if (backend->session()->onSetRate) {
        backend->session()->onSetRate(rate);
    }
    return 0;
}

static int prop_get_min_rate(sd_bus* bus, const char* path, const char* interface,
                             const char* property, sd_bus_message* reply,
                             void* userdata, sd_bus_error* error) {
    return sd_bus_message_append(reply, "d", 0.25);
}

static int prop_get_max_rate(sd_bus* bus, const char* path, const char* interface,
                             const char* property, sd_bus_message* reply,
                             void* userdata, sd_bus_error* error) {
    return sd_bus_message_append(reply, "d", 2.0);
}

static int prop_get_can_go_next(sd_bus* bus, const char* path, const char* interface,
                                const char* property, sd_bus_message* reply,
                                void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    return sd_bus_message_append(reply, "b", backend->canGoNext());
}

static int prop_get_can_go_previous(sd_bus* bus, const char* path, const char* interface,
                                    const char* property, sd_bus_message* reply,
                                    void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    return sd_bus_message_append(reply, "b", backend->canGoPrevious());
}

static int prop_get_can_play(sd_bus* bus, const char* path, const char* interface,
                             const char* property, sd_bus_message* reply,
                             void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    return sd_bus_message_append(reply, "b", backend->canPlay());
}

static int prop_get_can_pause(sd_bus* bus, const char* path, const char* interface,
                              const char* property, sd_bus_message* reply,
                              void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    return sd_bus_message_append(reply, "b", backend->canPause());
}

static int prop_get_can_seek(sd_bus* bus, const char* path, const char* interface,
                             const char* property, sd_bus_message* reply,
                             void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    return sd_bus_message_append(reply, "b", backend->canSeek());
}

static int prop_get_can_control(sd_bus* bus, const char* path, const char* interface,
                                const char* property, sd_bus_message* reply,
                                void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    return sd_bus_message_append(reply, "b", backend->canControl());
}

static int prop_get_metadata(sd_bus* bus, const char* path, const char* interface,
                             const char* property, sd_bus_message* reply,
                             void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    const auto& meta = backend->getMetadata();

    sd_bus_message_open_container(reply, 'a', "{sv}");

    // Track ID (required)
    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append(reply, "s", "mpris:trackid");
    sd_bus_message_open_container(reply, 'v', "o");
    sd_bus_message_append(reply, "o", "/org/jellyfin/track/1");
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    // Length
    if (meta.duration_us > 0) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "mpris:length");
        sd_bus_message_open_container(reply, 'v', "x");
        sd_bus_message_append(reply, "x", meta.duration_us);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    // Title
    if (!meta.title.empty()) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "xesam:title");
        sd_bus_message_open_container(reply, 'v', "s");
        sd_bus_message_append(reply, "s", meta.title.c_str());
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    // Artist (as array)
    if (!meta.artist.empty()) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "xesam:artist");
        sd_bus_message_open_container(reply, 'v', "as");
        sd_bus_message_append(reply, "as", 1, meta.artist.c_str());
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    // Album
    if (!meta.album.empty()) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "xesam:album");
        sd_bus_message_open_container(reply, 'v', "s");
        sd_bus_message_append(reply, "s", meta.album.c_str());
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    // Track number
    if (meta.track_number > 0) {
        sd_bus_message_open_container(reply, 'e', "sv");
        sd_bus_message_append(reply, "s", "xesam:trackNumber");
        sd_bus_message_open_container(reply, 'v', "i");
        sd_bus_message_append(reply, "i", meta.track_number);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }

    // Art URL
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

// Player interface methods
static int method_play(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    if (backend->session()->onPlay) backend->session()->onPlay();
    return sd_bus_reply_method_return(m, "");
}

static int method_pause(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    if (backend->session()->onPause) backend->session()->onPause();
    return sd_bus_reply_method_return(m, "");
}

static int method_play_pause(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    if (backend->session()->onPlayPause) backend->session()->onPlayPause();
    return sd_bus_reply_method_return(m, "");
}

static int method_stop(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    if (backend->session()->onStop) backend->session()->onStop();
    return sd_bus_reply_method_return(m, "");
}

static int method_next(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    if (backend->session()->onNext) backend->session()->onNext();
    return sd_bus_reply_method_return(m, "");
}

static int method_previous(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    if (backend->session()->onPrevious) backend->session()->onPrevious();
    return sd_bus_reply_method_return(m, "");
}

static int method_seek(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    int64_t offset;
    sd_bus_message_read(m, "x", &offset);
    int64_t new_pos = backend->getPosition() + offset;
    if (new_pos < 0) new_pos = 0;
    if (backend->session()->onSeek) backend->session()->onSeek(new_pos);
    return sd_bus_reply_method_return(m, "");
}

static int method_set_position(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    auto* backend = static_cast<MprisBackend*>(userdata);
    const char* track_id;
    int64_t position;
    sd_bus_message_read(m, "ox", &track_id, &position);
    if (backend->session()->onSeek) backend->session()->onSeek(position);
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
    SD_BUS_PROPERTY("Position", "x", prop_get_position, 0, 0),  // No signal for position
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

MprisBackend::MprisBackend(MediaSession* session, const std::string& service_suffix)
    : session_(session)
    , service_name_(std::string(BASE_SERVICE_NAME) + service_suffix) {
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

MprisBackend::~MprisBackend() {
    if (slot_player_) sd_bus_slot_unref(slot_player_);
    if (slot_root_) sd_bus_slot_unref(slot_root_);
    if (bus_) {
        sd_bus_release_name(bus_, service_name_.c_str());
        sd_bus_unref(bus_);
    }
}

void MprisBackend::setMetadata(const MediaMetadata& meta) {
    content_.metadata = meta;
    recomputeAndEmit();
}

void MprisBackend::setArtwork(const std::string& dataUri) {
    content_.metadata.art_data_uri = dataUri;
    recomputeAndEmit();
}

void MprisBackend::setPlaybackState(PlaybackState /*state*/) {
    // Authoritative state lives in the PlaybackCoordinator snapshot.
    // The sink call is a recompute trigger, not a state write.
    recomputeAndEmit();
}

void MprisBackend::setPosition(int64_t /*position_us*/) {
    // Position is polled, not signaled (per MPRIS spec). The single
    // source of truth is the coordinator snapshot — getPosition() reads
    // from there. No need to mirror it locally.
}

int64_t MprisBackend::getPosition() const {
    return g_playback_coord ? g_playback_coord->snapshot().position_us : 0;
}

void MprisBackend::setVolume(double volume) {
    content_.volume = volume;
    recomputeAndEmit();
}

void MprisBackend::setCanGoNext(bool can) {
    content_.can_go_next = can;
    recomputeAndEmit();
}

void MprisBackend::setCanGoPrevious(bool can) {
    content_.can_go_previous = can;
    recomputeAndEmit();
}

void MprisBackend::setRate(double rate) {
    content_.pending_rate = rate;
    recomputeAndEmit();
}

void MprisBackend::setBuffering(bool /*buffering*/) {
    // buffering lives in the coordinator snapshot.
    recomputeAndEmit();
}

void MprisBackend::setSeeking(bool /*seeking*/) {
    // seeking lives in the coordinator snapshot.
    recomputeAndEmit();
}

void MprisBackend::emitSeeked(int64_t position_us) {
    if (bus_)
        sd_bus_emit_signal(bus_, MPRIS_PATH, MPRIS_PLAYER_IFACE,
                           "Seeked", "x", position_us);
}

void MprisBackend::recomputeAndEmit() {
    PlaybackSnapshot snap = g_playback_coord ? g_playback_coord->snapshot()
                                             : PlaybackSnapshot{};
    MprisView next = project(snap, content_);
    auto changed = diff(last_, next);
    LOG_INFO(LOG_MEDIA,
        "mpris: snap phase={} buffering={} seeking={} -> status={} rate={} canSeek={} dur={} changed={}",
        static_cast<int>(snap.phase), snap.buffering, snap.seeking,
        next.playback_status.c_str(), next.rate, next.can_seek,
        next.metadata.duration_us, changed.size());
    last_ = std::move(next);
    if (!bus_ || changed.empty()) return;
    emitChanged(changed);
}

void MprisBackend::emitChanged(const std::vector<const char*>& names) {
    // sd_bus_emit_properties_changed_strv takes a NULL-terminated char**
    // array. Build the argv from the diff list.
    std::vector<char*> argv;
    argv.reserve(names.size() + 1);
    for (const char* n : names) argv.push_back(const_cast<char*>(n));
    argv.push_back(nullptr);
    sd_bus_emit_properties_changed_strv(bus_, MPRIS_PATH, MPRIS_PLAYER_IFACE,
                                        argv.data());
}

void MprisBackend::update() {
    if (!bus_) return;
    int r;
    do {
        r = sd_bus_process(bus_, nullptr);
    } while (r > 0);
}

int MprisBackend::getFd() {
    return bus_ ? sd_bus_get_fd(bus_) : -1;
}

std::unique_ptr<MediaSessionBackend> createMprisBackend(MediaSession* session, const std::string& service_suffix) {
    return std::make_unique<MprisBackend>(session, service_suffix);
}
