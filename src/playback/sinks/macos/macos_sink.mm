#include "macos_sink.h"

#include "../../../common.h"
#include "../../../browser/browsers.h"
#include "../../../browser/web_browser.h"
#include "../../../logging.h"

#include <dlfcn.h>
#include <condition_variable>
#include <mutex>

#import <AppKit/AppKit.h>
#import <MediaPlayer/MediaPlayer.h>

// macOS private visibility enum from MediaRemote.framework
enum {
    MRNowPlayingClientVisibilityUndefined = 0,
    MRNowPlayingClientVisibilityAlwaysVisible,
    MRNowPlayingClientVisibilityVisibleWhenBackgrounded,
    MRNowPlayingClientVisibilityNeverVisible
};

@interface MediaKeysDelegate : NSObject
@end

@implementation MediaKeysDelegate

- (instancetype)init {
    self = [super init];
    if (self) {
        MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];

        [center.playCommand addTarget:self action:@selector(handleCommand:)];
        [center.pauseCommand addTarget:self action:@selector(handleCommand:)];
        [center.togglePlayPauseCommand addTarget:self action:@selector(handleCommand:)];
        [center.stopCommand addTarget:self action:@selector(handleCommand:)];
        [center.nextTrackCommand addTarget:self action:@selector(handleCommand:)];
        [center.previousTrackCommand addTarget:self action:@selector(handleCommand:)];
        [center.changePlaybackPositionCommand addTarget:self action:@selector(handleSeek:)];
    }
    return self;
}

- (void)dealloc {
    MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];
    [center.playCommand removeTarget:self];
    [center.pauseCommand removeTarget:self];
    [center.togglePlayPauseCommand removeTarget:self];
    [center.stopCommand removeTarget:self];
    [center.nextTrackCommand removeTarget:self];
    [center.previousTrackCommand removeTarget:self];
    [center.changePlaybackPositionCommand removeTarget:self];
}

- (MPRemoteCommandHandlerStatus)handleCommand:(MPRemoteCommandEvent*)event {
    MPRemoteCommand* command = [event command];
    MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];

    if (command == center.playCommand) {
        g_mpv.Play();
    } else if (command == center.pauseCommand) {
        g_mpv.Pause();
    } else if (command == center.togglePlayPauseCommand) {
        g_mpv.TogglePause();
    } else if (command == center.stopCommand) {
        g_mpv.Stop();
    } else if (command == center.nextTrackCommand) {
        if (g_web_browser)
            g_web_browser->execJs("if(window._nativeHostInput) window._nativeHostInput(['next']);");
    } else if (command == center.previousTrackCommand) {
        if (g_web_browser)
            g_web_browser->execJs("if(window._nativeHostInput) window._nativeHostInput(['previous']);");
    } else {
        return MPRemoteCommandHandlerStatusCommandFailed;
    }
    return MPRemoteCommandHandlerStatusSuccess;
}

- (MPRemoteCommandHandlerStatus)handleSeek:(MPChangePlaybackPositionCommandEvent*)event {
    // Update Now Playing position immediately for responsive UI; rate=0
    // until mpv finishes the seek.
    NSMutableDictionary* info = [NSMutableDictionary
        dictionaryWithDictionary:[MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo];
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(event.positionTime);
    info[MPNowPlayingInfoPropertyPlaybackRate] = @(0.0);
    [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = info;

    int ms = static_cast<int>(event.positionTime * 1000.0);
    if (g_web_browser)
        g_web_browser->execJs("if(window._nativeSeek) window._nativeSeek(" + std::to_string(ms) + ");");
    return MPRemoteCommandHandlerStatusSuccess;
}

@end

MacosSink::MacosSink() = default;

MacosSink::~MacosSink() {
    stop();
}

void MacosSink::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&MacosSink::threadFunc, this);
}

void MacosSink::stop() {
    if (!running_.exchange(false)) return;
    wake().signal();
    if (thread_.joinable()) thread_.join();
}

void MacosSink::initRemote() {
    delegate_ = (__bridge_retained void*)[[MediaKeysDelegate alloc] init];

    media_remote_lib_ = dlopen("/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote", RTLD_NOW);
    if (media_remote_lib_) {
        SetNowPlayingVisibility_ = reinterpret_cast<SetNowPlayingVisibilityFunc>(
            dlsym(media_remote_lib_, "MRMediaRemoteSetNowPlayingVisibility"));
        GetLocalOrigin_ = reinterpret_cast<GetLocalOriginFunc>(
            dlsym(media_remote_lib_, "MRMediaRemoteGetLocalOrigin"));
        SetCanBeNowPlayingApplication_ = reinterpret_cast<SetCanBeNowPlayingApplicationFunc>(
            dlsym(media_remote_lib_, "MRMediaRemoteSetCanBeNowPlayingApplication"));
        if (SetCanBeNowPlayingApplication_)
            SetCanBeNowPlayingApplication_(1);
    } else {
        LOG_ERROR(LOG_MEDIA, "macOS Media: Failed to load MediaRemote.framework");
    }
}

void MacosSink::teardownRemote() {
    [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = nil;
    if (delegate_) {
        CFRelease(delegate_);
        delegate_ = nullptr;
    }
    if (media_remote_lib_) {
        dlclose(media_remote_lib_);
        media_remote_lib_ = nullptr;
    }
}

void MacosSink::threadFunc() {
    initRemote();

    // Block on the wake fd via cv-style poll. macOS WakeEvent has no fd
    // (Mach port-backed); use a 100ms cap loop.
    std::mutex local_mtx;
    std::unique_lock<std::mutex> lock(local_mtx);
    std::condition_variable local_cv;

    while (running_.load(std::memory_order_relaxed)) {
        wake().drain();
        pump();

        // Bounded wait — wake fd is signaled via WakeEvent::signal which
        // uses platform-appropriate primitive. We can't poll macOS Mach-port
        // fds with std::poll, so spin every 100ms checking running_ + drain.
        local_cv.wait_for(lock, std::chrono::milliseconds(100));
    }

    teardownRemote();
}

static MPNowPlayingPlaybackState convertState(PlaybackState state) {
    switch (state) {
        case PlaybackState::Playing: return MPNowPlayingPlaybackStatePlaying;
        case PlaybackState::Paused: return MPNowPlayingPlaybackStatePaused;
        case PlaybackState::Stopped: return MPNowPlayingPlaybackStateStopped;
        default: return MPNowPlayingPlaybackStateUnknown;
    }
}

static PlaybackState mapEventToState(PlaybackEvent::Kind k) {
    switch (k) {
    case PlaybackEvent::Kind::Started:     return PlaybackState::Playing;
    case PlaybackEvent::Kind::Paused:      return PlaybackState::Paused;
    case PlaybackEvent::Kind::TrackLoaded: return PlaybackState::Paused;
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error:       return PlaybackState::Stopped;
    default:                               return PlaybackState::Stopped;
    }
}

void MacosSink::deliver(const PlaybackEvent& ev) {
    switch (ev.kind) {
    case PlaybackEvent::Kind::MetadataChanged:
        // Same-Id dedup: a same-Id setMetadata is a no-op.
        if (!ev.metadata.id.empty() && ev.metadata.id == metadata_.id) break;
        metadata_ = ev.metadata;
        updateNowPlayingInfo();
        break;
    case PlaybackEvent::Kind::ArtworkChanged: {
        metadata_.art_data_uri = ev.artwork_uri;
        size_t comma = ev.artwork_uri.find(',');
        if (comma == std::string::npos) break;
        std::string base64Data = ev.artwork_uri.substr(comma + 1);
        NSString* nsBase64 = [NSString stringWithUTF8String:base64Data.c_str()];
        NSData* imageData = [[NSData alloc] initWithBase64EncodedString:nsBase64 options:0];
        if (!imageData) break;
        NSImage* image = [[NSImage alloc] initWithData:imageData];
        if (!image) break;
        MPMediaItemArtwork* artwork = [[MPMediaItemArtwork alloc]
            initWithBoundsSize:image.size
            requestHandler:^NSImage* _Nonnull(CGSize) {
                return image;
            }];
        NSMutableDictionary* info = [NSMutableDictionary
            dictionaryWithDictionary:[MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo];
        info[MPMediaItemPropertyArtwork] = artwork;
        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = info;
        break;
    }
    case PlaybackEvent::Kind::QueueCapsChanged:
        [MPRemoteCommandCenter sharedCommandCenter].nextTrackCommand.enabled = ev.can_go_next;
        [MPRemoteCommandCenter sharedCommandCenter].previousTrackCommand.enabled = ev.can_go_prev;
        break;
    case PlaybackEvent::Kind::Started:
    case PlaybackEvent::Kind::Paused:
    case PlaybackEvent::Kind::TrackLoaded:
    case PlaybackEvent::Kind::Finished:
    case PlaybackEvent::Kind::Canceled:
    case PlaybackEvent::Kind::Error: {
        PlaybackState st = mapEventToState(ev.kind);
        if (st == PlaybackState::Stopped) {
            metadata_ = MediaMetadata{};
            position_us_ = 0;
            [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = nil;
            [MPRemoteCommandCenter sharedCommandCenter].changePlaybackPositionCommand.enabled = NO;
        } else {
            [MPRemoteCommandCenter sharedCommandCenter].changePlaybackPositionCommand.enabled = YES;
        }
        [MPNowPlayingInfoCenter defaultCenter].playbackState = convertState(st);
        if (SetNowPlayingVisibility_ && GetLocalOrigin_) {
            void* origin = GetLocalOrigin_();
            SetNowPlayingVisibility_(origin,
                st == PlaybackState::Stopped
                    ? MRNowPlayingClientVisibilityNeverVisible
                    : MRNowPlayingClientVisibilityAlwaysVisible);
        }
        // State change forces an immediate timeline tick.
        if (st != PlaybackState::Stopped)
            updateTimelineThrottled(ev.snapshot.position_us, true);
        break;
    }
    case PlaybackEvent::Kind::PositionChanged:
        updateTimelineThrottled(ev.snapshot.position_us, false);
        break;
    case PlaybackEvent::Kind::RateChanged: {
        rate_ = ev.snapshot.rate;
        NSDictionary* existing = [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo;
        if (!existing) break;
        NSMutableDictionary* info = [NSMutableDictionary dictionaryWithDictionary:existing];
        info[MPNowPlayingInfoPropertyPlaybackRate] = @(rate_);
        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = info;
        break;
    }
    case PlaybackEvent::Kind::Seeked: {
        position_us_ = ev.snapshot.position_us;
        NSDictionary* existing = [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo;
        if (!existing) break;
        NSMutableDictionary* info = [NSMutableDictionary dictionaryWithDictionary:existing];
        info[MPNowPlayingInfoPropertyElapsedPlaybackTime] =
            @(static_cast<double>(position_us_) / 1000000.0);
        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = info;
        break;
    }
    case PlaybackEvent::Kind::SeekingChanged:
    case PlaybackEvent::Kind::BufferingChanged:
    case PlaybackEvent::Kind::DurationChanged:
    case PlaybackEvent::Kind::MediaTypeChanged:
    case PlaybackEvent::Kind::FullscreenChanged:
    case PlaybackEvent::Kind::OsdDimsChanged:
    case PlaybackEvent::Kind::BufferedRangesChanged:
    case PlaybackEvent::Kind::DisplayHzChanged:
        break;
    }
}

void MacosSink::updateTimelineThrottled(int64_t position_us, bool force) {
    position_us_ = position_us;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_position_update_).count();
    if (!force && elapsed < 1000) return;

    NSDictionary* existing = [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo;
    if (!existing) return;

    NSMutableDictionary* info = [NSMutableDictionary dictionaryWithDictionary:existing];
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] =
        @(static_cast<double>(position_us) / 1000000.0);
    [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = info;
    last_position_update_ = now;
}

void MacosSink::updateNowPlayingInfo() {
    NSMutableDictionary* info = [NSMutableDictionary dictionary];

    if (!metadata_.title.empty())
        info[MPMediaItemPropertyTitle] = [NSString stringWithUTF8String:metadata_.title.c_str()];
    if (!metadata_.artist.empty())
        info[MPMediaItemPropertyArtist] = [NSString stringWithUTF8String:metadata_.artist.c_str()];
    if (!metadata_.album.empty())
        info[MPMediaItemPropertyAlbumTitle] = [NSString stringWithUTF8String:metadata_.album.c_str()];
    if (metadata_.duration_us > 0)
        info[MPMediaItemPropertyPlaybackDuration] = @(static_cast<double>(metadata_.duration_us) / 1000000.0);
    if (metadata_.track_number > 0)
        info[MPMediaItemPropertyAlbumTrackNumber] = @(metadata_.track_number);

    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] =
        @(static_cast<double>(position_us_) / 1000000.0);
    info[MPNowPlayingInfoPropertyPlaybackRate] = @(rate_);

    MPNowPlayingInfoMediaType mpMediaType;
    switch (metadata_.media_type) {
        case MediaType::Audio:
            mpMediaType = MPNowPlayingInfoMediaTypeAudio; break;
        default:
            mpMediaType = MPNowPlayingInfoMediaTypeVideo; break;
    }
    info[MPNowPlayingInfoPropertyMediaType] = @(mpMediaType);

    [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = info;
}
