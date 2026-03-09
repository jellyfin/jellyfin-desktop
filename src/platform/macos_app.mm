// macOS NSApplication subclass implementing CefAppProtocol
// Must be initialized BEFORE SDL_Init for CEF compatibility

#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>  // For kCoreEventClass, kAEReopenApplication
#include "include/cef_application_mac.h"
#include <SDL3/SDL.h>

// Store main window reference for dock click handling
static NSWindow* g_mainWindow = nil;

@interface JellyfinApplication : NSApplication <CefAppProtocol> {
    BOOL handlingSendEvent_;
}
@end

@implementation JellyfinApplication

- (instancetype)init {
    self = [super init];
    if (self) {
        // Register for reopen Apple Event (dock icon click)
        [[NSAppleEventManager sharedAppleEventManager]
            setEventHandler:self
                andSelector:@selector(handleReopenEvent:withReplyEvent:)
              forEventClass:kCoreEventClass
                 andEventID:kAEReopenApplication];
    }
    return self;
}

- (void)handleReopenEvent:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply {
    if (g_mainWindow && [g_mainWindow isMiniaturized]) {
        [g_mainWindow deminiaturize:nil];
    }
}

- (BOOL)isHandlingSendEvent {
    return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
    handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
    CefScopedSendingEvent sendingEventScoper;
    [super sendEvent:event];
}

- (void)terminate:(id)sender {
    // Don't call [super terminate:] which calls exit() and crashes during static destruction.
    // Instead, post SDL quit event to let main loop handle cleanup properly.
    SDL_Event event;
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

@end

void initMacApplication() {
    // Create our CEF-compatible NSApplication subclass
    // This must be done before SDL_Init, which would create a plain NSApplication
    [JellyfinApplication sharedApplication];

    // Helper processes (GPU, renderer) only need CefAppProtocol - skip dock/menu setup
    bool is_subprocess = (getenv("JELLYFIN_CEF_SUBPROCESS") != nullptr);
    if (!is_subprocess) {
        // Make this a foreground app (shows in dock, gets menubar, receives keyboard)
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Create basic menu bar with Quit item for Cmd+Q
        NSMenu* menubar = [[NSMenu alloc] init];
        NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
        [menubar addItem:appMenuItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                          action:@selector(terminate:)
                                                   keyEquivalent:@"q"];
        [appMenu addItem:quitItem];
        [appMenuItem setSubmenu:appMenu];
        [NSApp setMainMenu:menubar];
    }

    NSLog(@"NSApplication class: %@", NSStringFromClass([NSApp class]));
    NSLog(@"Conforms to CefAppProtocol: %@", [NSApp conformsToProtocol:@protocol(CefAppProtocol)] ? @"YES" : @"NO");
}

// Wait for NSApplication events (integrates with both Cocoa and CFRunLoop)
// Doesn't dequeue - just waits until an event is available, then returns
// so SDL can process it
void waitForMacEvent() {
    @autoreleasepool {
        // Wait indefinitely for any event, but don't dequeue it
        // This pumps CFRunLoop (processing Mojo IPC) while waiting
        [NSApp nextEventMatchingMask:NSEventMaskAny
                           untilDate:[NSDate distantFuture]
                              inMode:NSDefaultRunLoopMode
                             dequeue:NO];
        // Event stays in queue for SDL to process
    }
}

// Wake the NSApplication event loop from another thread
void wakeMacEventLoop() {
    @autoreleasepool {
        NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSMakePoint(0, 0)
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0];
        [NSApp postEvent:event atStart:YES];
    }
}

// Call this after SDL window is created to ensure it can receive keyboard input
void activateMacWindow(SDL_Window* window) {
    [NSApp activateIgnoringOtherApps:YES];

    // Get NSWindow directly from SDL (don't rely on [NSApp mainWindow] being set yet)
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    NSWindow* ns_window = (__bridge NSWindow*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (ns_window) {
        [ns_window makeKeyAndOrderFront:nil];
        // Store for dock click handling
        g_mainWindow = ns_window;
    }
}
