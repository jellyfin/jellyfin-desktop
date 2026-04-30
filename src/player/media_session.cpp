#include "player/media_session.h"
#include "common.h"
#include "cef/message_bus.h"

#ifdef __APPLE__
#include "player/macos/media_session_macos.h"
#elif defined(_WIN32)
#include "player/windows/media_session_windows.h"
#else
#include "player/mpris/media_session_mpris.h"
#endif

MediaSession::MediaSession() = default;

MediaSession::~MediaSession() = default;

std::unique_ptr<MediaSession> MediaSession::create() {
    auto session = std::make_unique<MediaSession>();
#ifdef __APPLE__
    session->addBackend(createMacOSMediaBackend(session.get()));
#elif defined(_WIN32)
    int64_t wid = 0;
    g_mpv.GetPropertyInt("window-id", wid);
    session->addBackend(createWindowsMediaBackend(session.get(), (HWND)(intptr_t)wid));
#else
    session->addBackend(std::make_unique<MprisBackend>(session.get()));
#endif
    session->wireTransportCallbacks();
    return session;
}

void MediaSession::wireTransportCallbacks() {
    onPlay = []() { g_mpv.Play(); };
    onPause = []() { g_mpv.Pause(); };
    onPlayPause = []() { g_mpv.TogglePause(); };
    onStop = []() { g_mpv.Stop(); };
    onSetRate = [](double rate) { g_mpv.SetSpeed(rate); };
    onNext = []() {
        auto p = CefDictionaryValue::Create();
        auto actions = CefListValue::Create();
        actions->SetString(0, "next");
        p->SetList("actions", actions);
        g_bus.emit("input.hostInput", p);
    };
    onPrevious = []() {
        auto p = CefDictionaryValue::Create();
        auto actions = CefListValue::Create();
        actions->SetString(0, "previous");
        p->SetList("actions", actions);
        g_bus.emit("input.hostInput", p);
    };
    onSeek = [](int64_t position_us) {
        auto p = CefDictionaryValue::Create();
        p->SetInt("positionMs", static_cast<int>(position_us / 1000));
        g_bus.emit("input.positionSeek", p);
    };
}

void MediaSession::setMetadata(const MediaMetadata& meta) {
    for (auto& b : backends_) b->setMetadata(meta);
}

void MediaSession::setArtwork(const std::string& dataUri) {
    for (auto& b : backends_) b->setArtwork(dataUri);
}

void MediaSession::setPlaybackState(PlaybackState state) {
    state_ = state;
    for (auto& b : backends_) b->setPlaybackState(state);
}

void MediaSession::setPosition(int64_t position_us) {
    for (auto& b : backends_) b->setPosition(position_us);
}

void MediaSession::setVolume(double volume) {
    for (auto& b : backends_) b->setVolume(volume);
}

void MediaSession::setCanGoNext(bool can) {
    for (auto& b : backends_) b->setCanGoNext(can);
}

void MediaSession::setCanGoPrevious(bool can) {
    for (auto& b : backends_) b->setCanGoPrevious(can);
}

void MediaSession::setRate(double rate) {
    for (auto& b : backends_) b->setRate(rate);
}

void MediaSession::setBuffering(bool buffering) {
    for (auto& b : backends_) b->setBuffering(buffering);
}

void MediaSession::emitSeeking() {
    for (auto& b : backends_) b->emitSeeking();
}

void MediaSession::emitSeeked(int64_t position_us) {
    for (auto& b : backends_) b->emitSeeked(position_us);
}

void MediaSession::update() {
    for (auto& b : backends_) b->update();
}

int MediaSession::getFd() {
    for (auto& b : backends_) {
        int fd = b->getFd();
        if (fd >= 0) return fd;
    }
    return -1;
}
