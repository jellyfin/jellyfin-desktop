#include "player/media_session.h"

MediaSession::MediaSession(std::unique_ptr<MediaSessionBackend> backend) {
    if (backend) backends_.push_back(std::move(backend));
}

MediaSession::~MediaSession() = default;

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
