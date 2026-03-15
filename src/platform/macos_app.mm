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

        // Dark titlebar with transparent chrome so backgroundColor shows through
        ns_window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        ns_window.titlebarAppearsTransparent = YES;
        ns_window.backgroundColor = [NSColor colorWithSRGBRed:0x10 / 255.0
                                                        green:0x10 / 255.0
                                                         blue:0x10 / 255.0
                                                        alpha:1.0];
    }
}

void setMacTitlebarColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_mainWindow) return;
    g_mainWindow.backgroundColor = [NSColor colorWithSRGBRed:r / 255.0
                                                       green:g / 255.0
                                                        blue:b / 255.0
                                                       alpha:1.0];
}
