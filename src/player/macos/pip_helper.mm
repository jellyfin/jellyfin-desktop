#ifdef __APPLE__
#include "pip_helper.h"
#include "logging.h"

#import <AppKit/AppKit.h>

// ---------------------------------------------------------------------------
// Private PIP.framework API (same approach as IINA)
// ---------------------------------------------------------------------------
@interface PIPViewController : NSViewController
@property (nonatomic, copy, nullable) NSString *name;
@property (nonatomic, weak, nullable) id delegate;
@property (nonatomic, weak, nullable) NSWindow *replacementWindow;
@property (nonatomic) NSRect replacementRect;
@property (nonatomic) bool playing;
@property (nonatomic) NSSize aspectRatio;
- (void)presentViewControllerAsPictureInPicture:(NSViewController *)viewController;
@end

@protocol PIPViewControllerDelegateProtocol <NSObject>
@optional
- (BOOL)pipShouldClose:(PIPViewController *)pip;
- (void)pipWillClose:(PIPViewController *)pip;
- (void)pipDidClose:(PIPViewController *)pip;
- (void)pipActionPlay:(PIPViewController *)pip;
- (void)pipActionPause:(PIPViewController *)pip;
- (void)pipActionStop:(PIPViewController *)pip;
@end

// ---------------------------------------------------------------------------
// Delegate: receives PiP lifecycle events
// ---------------------------------------------------------------------------
@interface PiPDelegate : NSObject <PIPViewControllerDelegateProtocol>
@property (nonatomic, copy) void (^playBlock)(void);
@property (nonatomic, copy) void (^pauseBlock)(void);
@property (nonatomic, copy) void (^restoreBlock)(void);
@end

@implementation PiPDelegate

- (BOOL)pipShouldClose:(PIPViewController *)pip {
    LOG_INFO(LOG_PLATFORM, "PiP: should close");
    return YES;
}

- (void)pipWillClose:(PIPViewController *)pip {
    LOG_INFO(LOG_PLATFORM, "PiP: will close");
}

- (void)pipDidClose:(PIPViewController *)pip {
    LOG_INFO(LOG_PLATFORM, "PiP: did close");
    if (self.restoreBlock) {
        self.restoreBlock();
    }
}

- (void)pipActionPlay:(PIPViewController *)pip {
    LOG_INFO(LOG_PLATFORM, "PiP: play");
    pip.playing = YES;
    if (self.playBlock) {
        self.playBlock();
    }
}

- (void)pipActionPause:(PIPViewController *)pip {
    LOG_INFO(LOG_PLATFORM, "PiP: pause");
    pip.playing = NO;
    if (self.pauseBlock) {
        self.pauseBlock();
    }
}

- (void)pipActionStop:(PIPViewController *)pip {
    LOG_INFO(LOG_PLATFORM, "PiP: stop");
    pip.playing = NO;
    if (self.pauseBlock) {
        self.pauseBlock();
    }
}

@end

// ---------------------------------------------------------------------------
// Implementation struct
// ---------------------------------------------------------------------------
struct MacOSPiPHelper::Impl {
    PIPViewController* pipController = nil;
    NSViewController* videoVC = nil;
    PiPDelegate* delegate = nil;
    bool active = false;
    std::function<void(bool)> playPauseCb;
    std::function<void()> restoreCb;
};

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

MacOSPiPHelper::MacOSPiPHelper() : impl_(new Impl()) {}

MacOSPiPHelper::~MacOSPiPHelper() {
    if (impl_) {
        stop();
        delete impl_;
    }
}

bool MacOSPiPHelper::isSupported() {
    // PIPViewController is available since macOS 10.12
    return NSClassFromString(@"PIPViewController") != nil;
}

void MacOSPiPHelper::start(void* videoView, int videoW, int videoH) {
    if (!isSupported() || !videoView) return;
    if (impl_->active) return;

    NSView* view = (__bridge NSView*)videoView;

    // Create PiP controller
    impl_->pipController = [[PIPViewController alloc] init];

    // Create delegate
    impl_->delegate = [[PiPDelegate alloc] init];
    if (impl_->playPauseCb) {
        auto cb = impl_->playPauseCb;
        impl_->delegate.playBlock = ^{ cb(true); };
        impl_->delegate.pauseBlock = ^{ cb(false); };
    }
    if (impl_->restoreCb) {
        auto restoreCb = impl_->restoreCb;
        impl_->delegate.restoreBlock = ^{ restoreCb(); };
    }

    impl_->pipController.delegate = impl_->delegate;
    impl_->pipController.playing = YES;
    impl_->pipController.aspectRatio = NSMakeSize(videoW, videoH);

    // Set replacement window for the fly-back animation
    impl_->pipController.replacementWindow = view.window;
    impl_->pipController.replacementRect = view.window.contentView.frame;

    // Wrap the video view in a view controller and present as PiP
    impl_->videoVC = [[NSViewController alloc] init];
    impl_->videoVC.view = view;
    [impl_->pipController presentViewControllerAsPictureInPicture:impl_->videoVC];

    impl_->active = true;
    LOG_INFO(LOG_PLATFORM, "PiP: started (%dx%d)", videoW, videoH);
}

void MacOSPiPHelper::stop() {
    if (!impl_->active) return;
    if (impl_->pipController && impl_->videoVC) {
        [impl_->pipController dismissViewController:impl_->videoVC];
    }
    impl_->pipController = nil;
    impl_->videoVC = nil;
    impl_->delegate = nil;
    impl_->active = false;
    LOG_INFO(LOG_PLATFORM, "PiP: stopped");
}

void MacOSPiPHelper::toggle(void* videoView, int videoW, int videoH) {
    if (impl_->active) {
        stop();
    } else {
        start(videoView, videoW, videoH);
    }
}

bool MacOSPiPHelper::isActive() const {
    return impl_->active;
}

void MacOSPiPHelper::setAspectRatio(int videoW, int videoH) {
    if (impl_->pipController && videoW > 0 && videoH > 0) {
        impl_->pipController.aspectRatio = NSMakeSize(videoW, videoH);
    }
}

void MacOSPiPHelper::setPlaying(bool playing) {
    if (impl_->pipController) {
        impl_->pipController.playing = playing;
    }
}

void MacOSPiPHelper::setPlayPauseCallback(std::function<void(bool playing)> cb) {
    impl_->playPauseCb = std::move(cb);
}

void MacOSPiPHelper::setRestoreCallback(std::function<void()> cb) {
    impl_->restoreCb = std::move(cb);
}

#endif // __APPLE__
