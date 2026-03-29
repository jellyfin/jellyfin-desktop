// macOS NSApplication subclass implementing CefAppProtocol
// Must be initialized BEFORE SDL_Init for CEF compatibility

#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>  // For kCoreEventClass, kAEReopenApplication
#include "include/cef_application_mac.h"
#include "settings.h"
#include <SDL3/SDL.h>
#include <cmath>

// Store main window reference for dock click handling
static NSWindow* g_mainWindow = nil;
static NSView* g_titlebarDragView = nil;
static bool g_trafficLightsVisible = true;
static const CGFloat kTitlebarHeight = 28.0;
using NativeScrollHandler = void(*)(int x, int y, float deltaX, float deltaY);
static NativeScrollHandler g_nativeScrollHandler = nullptr;

// Apply current traffic light visibility state to window buttons and drag view.
// Uses alphaValue instead of setHidden: because AppKit can reset hidden state
// when redrawing standard window buttons during activation changes.
static void applyTrafficLightsVisibility() {
    if (!g_mainWindow) return;
    CGFloat alpha = g_trafficLightsVisible ? 1.0 : 0.0;
    [g_mainWindow standardWindowButton:NSWindowCloseButton].alphaValue = alpha;
    [g_mainWindow standardWindowButton:NSWindowMiniaturizeButton].alphaValue = alpha;
    [g_mainWindow standardWindowButton:NSWindowZoomButton].alphaValue = alpha;
    if (g_titlebarDragView) {
        [g_titlebarDragView setHidden:!g_trafficLightsVisible];
    }
}

// Transparent view covering the titlebar area that intercepts mouse events
// and initiates window dragging, preventing clicks from reaching CEF.
@interface TitlebarDragView : NSView
@end

@implementation TitlebarDragView

- (void)mouseDown:(NSEvent*)event {
    [self.window performWindowDragWithEvent:event];
}

- (void)mouseUp:(NSEvent*)event {
    // Swallow — don't pass to views underneath
}

// Accept first mouse so clicks on inactive windows also drag (not forwarded to CEF)
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}

@end

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
    if (event.type == NSEventTypeScrollWheel && g_nativeScrollHandler) {
        // Forward the original Cocoa scroll event deltas to the active browser
        // before SDL translates them into wheel events.
        NSWindow* window = event.window ?: g_mainWindow;
        NSView* content = window.contentView;
        if (content) {
            NSPoint point = [content convertPoint:event.locationInWindow fromView:nil];
            if (NSPointInRect(point, content.bounds)) {
                int x = static_cast<int>(lround(point.x));
                int y = static_cast<int>(lround(content.bounds.size.height - point.y));
                g_nativeScrollHandler(x, y,
                                      static_cast<float>(event.scrollingDeltaX),
                                      static_cast<float>(event.scrollingDeltaY));
            }
        }
    }
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

    // Helper processes (GPU, renderer) only need CefAppProtocol - hide from dock
    bool is_subprocess = (getenv("JELLYFIN_CEF_SUBPROCESS") != nullptr);
    if (is_subprocess) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        return;
    }

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

    NSLog(@"NSApplication class: %@", NSStringFromClass([NSApp class]));
    NSLog(@"Conforms to CefAppProtocol: %@", [NSApp conformsToProtocol:@protocol(CefAppProtocol)] ? @"YES" : @"NO");
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

        ns_window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        ns_window.backgroundColor = [NSColor colorWithSRGBRed:0x10 / 255.0
                                                        green:0x10 / 255.0
                                                         blue:0x10 / 255.0
                                                        alpha:1.0];

        if (Settings::instance().transparentTitlebar()) {
            // Transparent titlebar overlaying window content (traffic lights float over web UI)
            ns_window.styleMask |= NSWindowStyleMaskFullSizeContentView;
            ns_window.titlebarAppearsTransparent = YES;
            ns_window.titleVisibility = NSWindowTitleHidden;

            // Add transparent drag view over the titlebar area to prevent mouse events
            // from reaching CEF and enable window dragging by click-drag.
            NSView* content = ns_window.contentView;
            NSRect dragFrame = NSMakeRect(0,
                                          content.bounds.size.height - kTitlebarHeight,
                                          content.bounds.size.width,
                                          kTitlebarHeight);
            g_titlebarDragView = [[TitlebarDragView alloc] initWithFrame:dragFrame];
            g_titlebarDragView.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
            [content addSubview:g_titlebarDragView positioned:NSWindowAbove relativeTo:nil];

            // macOS redraws standard window buttons on activation changes, which can
            // reset their hidden state. Re-apply after each transition.
            NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
            [nc addObserverForName:NSWindowDidBecomeMainNotification object:ns_window queue:nil
                        usingBlock:^(NSNotification*) { applyTrafficLightsVisibility(); }];
            [nc addObserverForName:NSWindowDidResignMainNotification object:ns_window queue:nil
                        usingBlock:^(NSNotification*) { applyTrafficLightsVisibility(); }];
        } else {
            // Colored titlebar with standard chrome
            ns_window.titlebarAppearsTransparent = YES;
        }
    }
}

void setMacTitlebarColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_mainWindow) return;
    g_mainWindow.backgroundColor = [NSColor colorWithSRGBRed:r / 255.0
                                                       green:g / 255.0
                                                        blue:b / 255.0
                                                       alpha:1.0];
}

void setMacTrafficLightsVisible(bool visible) {
    if (!g_mainWindow || visible == g_trafficLightsVisible) return;
    g_trafficLightsVisible = visible;
    applyTrafficLightsVisibility();
}

void setMacNativeScrollHandler(NativeScrollHandler handler) {
    g_nativeScrollHandler = handler;
}

bool macNativeScrollBridgeEnabled() {
    return g_nativeScrollHandler != nullptr;
}
