// platform_macos.mm — macOS platform layer.
// CEF content composites onto mpv's window as a stack of CAMetalLayers,
// one per CefLayer (allocated by macos_alloc_surface, destroyed by
// macos_free_surface). CEF delivers straight-alpha BGRA via
// OnAcceleratedPaint; a Metal pass converts to premultiplied alpha and
// renders into the layer's nextDrawable. CAMetalLayer.colorspace = sRGB
// tells CoreAnimation how to color-manage the content into the window's
// working space (P3/EDR). Input is owned by src/input/input_macos.mm.

#include "platform/platform.h"
#include "platform/macos_platform.h"
#include "common.h"
#include "browser/browsers.h"
#include "browser/about_browser.h"
#include "cef/cef_app.h"
#include "cef/cef_client.h"
#include "logging.h"
#include "mpv/jfn_mpv_api.h"

#include "include/cef_application_mac.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <algorithm>
#include <vector>

// SCDynamicStoreCopyComputerName returns the freeform "Computer Name" from
// System Settings — which can contain emoji, smart quotes, CJK, and other
// non-ASCII that breaks the HTTP header.
// SCDynamicStoreCopyLocalHostName returns the Bonjour hostname: always DNS-safe ASCII (letters, digits,
// hyphens), derived from the Computer Name by macOS itself.
std::string macosComputerName() {
    CFStringRef name = SCDynamicStoreCopyLocalHostName(nullptr);
    if (!name) return {};
    CFIndex len = CFStringGetLength(name);
    CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(max, '\0');
    CFStringGetCString(name, out.data(), max, kCFStringEncodingUTF8);
    out.resize(strlen(out.c_str()));
    CFRelease(name);
    return out;
}

// =====================================================================
// Forward declarations
// =====================================================================

extern "C" void macos_pump();

// =====================================================================
// JellyfinApplication — NSApplication subclass required by CEF
// =====================================================================

@interface JellyfinApplication : NSApplication <CefAppProtocol> {
    BOOL handlingSendEvent_;
}
@end

@implementation JellyfinApplication

- (instancetype)init {
    self = [super init];
    if (self) {
        [[NSAppleEventManager sharedAppleEventManager]
            setEventHandler:self
                andSelector:@selector(handleReopenEvent:withReplyEvent:)
              forEventClass:kCoreEventClass
                 andEventID:kAEReopenApplication];
    }
    return self;
}

- (void)handleReopenEvent:(NSAppleEventDescriptor*)event
           withReplyEvent:(NSAppleEventDescriptor*)reply {
    for (NSWindow* w in [NSApp windows]) {
        if ([w isMiniaturized]) { [w deminiaturize:nil]; break; }
    }
}

- (BOOL)isHandlingSendEvent { return handlingSendEvent_; }
- (void)setHandlingSendEvent:(BOOL)v { handlingSendEvent_ = v; }
- (void)sendEvent:(NSEvent*)event {
    CefScopedSendingEvent sendingEventScoper;
    [super sendEvent:event];
}
- (void)terminate:(id)sender {
    initiate_shutdown();
}

@end

// =====================================================================
// Per-surface compositor state, surface registry, Metal device/queue/
// pipeline, and the expected-size transition gate all live in the Rust
// jfn-macos crate (src/macos/src/lib.rs). The C++ side here keeps only
// the NSWindow* / input-view pair set up by macos_init below and the
// objects that haven't been ported yet (popup, fullscreen, NSApp menus).
// PlatformSurface itself is opaque to this file.
// =====================================================================

// Input NSView (owned by jfn-macos input.rs)
static NSView* g_input_view = nil;

// jfn_input_macos_create_view returns a +1-retained NSView from the
// Rust input crate. With ARC enabled here we adopt it via
// __bridge_transfer to balance the retain.
extern "C" void* jfn_input_macos_create_view();

// Window
static NSWindow* g_window = nullptr;

// CADisplayLink drives CEF BeginFrame production synchronized with the
// real display refresh. The callback fires on the main runloop each
// vsync and calls SendExternalBeginFrame on each browser whose host is
// ready. CEF produces a frame immediately if its compositor has
// invalidation, or does nothing if not — no polling, no wasted work.
static CADisplayLink* g_display_link = nil;

// =====================================================================
// CADisplayLink → CEF BeginFrame
// =====================================================================

// CADisplayLink target — fires on the main runloop at the display's
// refresh rate, driving CEF's external BeginFrame production.
@interface DisplayLinkTarget : NSObject
- (void)tick:(CADisplayLink*)link;
@end

@implementation DisplayLinkTarget
- (void)tick:(CADisplayLink*)link {
    (void)link;
    if (jfn_shutting_down()) return;
    if (g_browsers) {
        for (auto& layer : g_browsers->layers()) {
            layer->sendExternalBeginFrame();
        }
    }
}
@end

static DisplayLinkTarget* g_display_link_target = nil;

static bool start_display_link() {
    g_display_link_target = [[DisplayLinkTarget alloc] init];
    g_display_link = [[g_window screen] displayLinkWithTarget:g_display_link_target
                                                     selector:@selector(tick:)];
    if (!g_display_link) {
        LOG_ERROR(LOG_PLATFORM, "[CVDL] displayLinkWithTarget failed");
        return false;
    }
    [g_display_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    LOG_INFO(LOG_PLATFORM, "[CVDL] started");
    return true;
}

static void stop_display_link() {
    if (!g_display_link) return;
    [g_display_link invalidate];
    g_display_link = nil;
    g_display_link_target = nil;
    LOG_INFO(LOG_PLATFORM, "[CVDL] stopped");
}

// =====================================================================
// Platform interface implementation
// =====================================================================

// macos_set_theme_color now lives in src/macos/src/lib.rs. It reaches the
// NSWindow* via jfn_macos_get_window() below and re-enters via
// jfn_macos_apply_theme_color_on_main on the main queue.
extern "C" void macos_set_theme_color(uint32_t rgb);

// Reached by Rust on the main thread (either inline or via the dispatch
// queue) to write the NSColor onto g_window + its content layer.
extern "C" void jfn_macos_apply_theme_color_on_main(uint32_t rgb) {
    Color c{rgb};
    NSColor* ns = [NSColor colorWithSRGBRed:c.r/255.0 green:c.g/255.0
                                       blue:c.b/255.0 alpha:1.0];
    if (!g_window) return;
    g_window.backgroundColor = ns;
    if (NSView* cv = [g_window contentView]; cv.layer)
        cv.layer.backgroundColor = ns.CGColor;
}

// Narrow accessor for Rust crate to reach the AppKit NSWindow*. Returns
// nullptr before macos_init has located mpv's window or after cleanup.
extern "C" void* jfn_macos_get_window() {
    return (__bridge void*)g_window;
}

// Narrow accessor for the input NSView — owned by input::macos and
// installed by macos_init below. Rust's macos_restack re-anchors it on
// top of the CefLayer subviews after any reorder.
extern "C" void* jfn_macos_get_input_view() {
    return (__bridge void*)g_input_view;
}

// Rust crate (src/macos/src/lib.rs) owns the per-surface compositor
// state (surface registry, Metal device/queue/pipeline, expected-size
// transition gate). Called from macos_cleanup below to tear down the
// Rust side before we drop the NSWindow.
extern "C" void jfn_macos_compositor_cleanup();

extern "C" bool macos_init(mpv_handle* mpv) {
    LOG_INFO(LOG_PLATFORM, "[INIT] macos_init: waiting for mpv window");
    for (int i = 0; i < 500 && !g_window; i++) {
        macos_pump();
        for (NSWindow* w in [NSApp windows]) {
            if ([w isVisible]) { g_window = w; break; }
        }
        if (!g_window) usleep(10000);
    }
    if (!g_window) {
        LOG_ERROR(LOG_PLATFORM, "[INIT] mpv did not create a window");
        return false;
    }
    LOG_INFO(LOG_PLATFORM, "[INIT] macos_init: got window={}", (__bridge void*)g_window);

    // mpv's Window.windowShouldClose sends MP_KEY_CLOSE_WIN into mpv's
    // input system, which we've disabled. Swizzle it to call our shutdown.
    {
        Class cls = [g_window class];
        SEL sel = @selector(windowShouldClose:);
        IMP newImp = imp_implementationWithBlock(^BOOL(id, NSWindow*) {
            initiate_shutdown();
            return NO;
        });
        Method m = class_getInstanceMethod(cls, sel);
        method_setImplementation(m, newImp);
    }

    // The first reconfig already applied --geometry (including position via
    // --force-window-position). Clear it so subsequent reconfigs (video
    // start/stop) don't reposition+resize the window.
    jfn_mpv_set_force_window_position(false);

    // Dock icon
    NSString* iconPath = [[[NSBundle mainBundle] resourcePath]
        stringByAppendingPathComponent:@"AppIcon.icns"];
    NSImage* icon = [[NSImage alloc] initWithContentsOfFile:iconPath];
    if (icon) [NSApp setApplicationIconImage:icon];

    // Transparent titlebar
    g_window.titlebarAppearsTransparent = YES;
    g_window.titleVisibility = NSWindowTitleHidden;
    g_window.styleMask |= NSWindowStyleMaskFullSizeContentView;

    NSView* contentView = [g_window contentView];
    if (!contentView.layer) [contentView setWantsLayer:YES];

    // Cover the AppKit fills before CEF delivers its first frame; ThemeColor
    // takes over from overlay-dismissal onward.
    macos_set_theme_color(kBgColor.rgb);

    // Metal device/queue/pipeline are created lazily on first
    // macos_alloc_surface inside the Rust jfn-macos crate.

    // CefLayer surfaces are created on demand via macos_alloc_surface and
    // ordered by macos_restack. The input NSView sits above whatever
    // CefLayer subviews currently exist; macos_restack re-anchors it on
    // top after any reorder.
    CGRect frame = [contentView bounds];

    g_input_view = (__bridge_transfer NSView*)jfn_input_macos_create_view();
    g_input_view.frame = contentView.bounds;
    g_input_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [contentView addSubview:g_input_view];

    // NSWindow drops mouseMoved: events on the floor unless this is set.
    // Without it, our input view's hover/cursor tracking never fires.
    g_window.acceptsMouseMovedEvents = YES;

    // Put the input view in the responder chain so keyDown:/keyUp: reach it.
    // Without an explicit makeFirstResponder:, the window's first responder
    // is whatever mpv's VO set up — typically mpv's own rendering view, which
    // doesn't forward to us.
    [g_window makeFirstResponder:g_input_view];

    // Diagnostic: log Cocoa-level resize notifications so we can tell whether
    // the OS is reporting resizes at all (independent of mpv's osd-dimensions
    // property propagation through the digest thread).
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSWindowDidResizeNotification
                    object:g_window
                     queue:nil
                usingBlock:^(NSNotification* /*note*/) {
        NSRect b = [[g_window contentView] bounds];
        LOG_TRACE(LOG_PLATFORM, "[WINDOW] NSWindowDidResizeNotification contentView={:.0f}x{:.0f}",
                 b.size.width, b.size.height);
    }];

    // Start the display link. This drives CEF BeginFrame production at
    // the display's real refresh rate; without it (external_begin_frame
    // = true but no caller) CEF produces no frames at all.
    if (!start_display_link()) {
        LOG_ERROR(LOG_PLATFORM, "[INIT] failed to start CADisplayLink");
        return false;
    }

    LOG_INFO(LOG_PLATFORM, "[INIT] Metal compositor initialized frame={:.0f}x{:.0f} window.firstResponder={} input_view={}",
             frame.size.width, frame.size.height,
             (__bridge void*)[g_window firstResponder], (__bridge void*)g_input_view);
    return true;
}

// macos_popup_show now lives in src/macos/src/popup.rs.

// macos_surface_set_visible and macos_fade_surface now live in
// src/macos/src/lib.rs.

// macos_set_fullscreen + macos_toggle_fullscreen now live in
// src/macos/src/lib.rs.

// macos_begin_transition, macos_in_transition, macos_end_transition,
// macos_drop_input_textures and macos_set_expected_size all now live in
// src/macos/src/lib.rs.

// macos_get_scale now lives in src/macos/src/lib.rs.

// macos_get_display_scale now lives in src/macos/src/lib.rs.

namespace macos_platform {
bool query_logical_content_size(int* w, int* h) {
    if (!g_window) return false;
    NSRect bounds = [[g_window contentView] bounds];
    *w = static_cast<int>(bounds.size.width);
    *h = static_cast<int>(bounds.size.height);
    return *w > 0 && *h > 0;
}
}

// macos_query_window_position now lives in src/macos/src/lib.rs.

// macos_clamp_window_geometry now lives in src/macos/src/lib.rs.

// macos_pump, macos_run_main_loop, and macos_wake_main_loop now live in
// src/macos/src/lib.rs.

extern "C" void macos_cleanup() {
    // Stop the display link first so no more BeginFrames race the teardown.
    stop_display_link();

    if (g_input_view) { [g_input_view removeFromSuperview]; g_input_view = nil; }

    // Tear down per-surface compositor state (surface registry, Metal
    // device/queue/pipeline) inside the Rust crate.
    jfn_macos_compositor_cleanup();

    g_window = nil;
}

// Target for the app menu's "About" item. Lives for the process lifetime,
// matches the pattern JellyfinPopupMenuTarget uses for <select> menus.
@interface JellyfinAppMenuTarget : NSObject
- (void)showAbout:(id)sender;
@end

@implementation JellyfinAppMenuTarget
- (void)showAbout:(id)sender {
    (void)sender;
    AboutBrowser::open();
}
@end

static JellyfinAppMenuTarget* g_app_menu_target = nil;

extern "C" void macos_early_init() {
    [JellyfinApplication sharedApplication];

    // Subprocesses (GPU, renderer) only need CefAppProtocol — hide from dock
    if (getenv("JELLYFIN_CEF_SUBPROCESS")) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        return;
    }

    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    // AppKit can add Dictation and Character Palette items to standard Edit
    // menus. Apple documents Emoji & Symbols as Fn/Globe-E or Edit > Emoji &
    // Symbols
    //
    // That shortcut is hard to work with for our input architecture. We receive the
    // original NSEvent in JellyfinInputView, translate it into CefKeyEvent, and
    // inject it into an off-screen CEF browser. CEF's public event flags do not
    // include a Function/Globe bit, only caps/shift/control/option/command/etc.:
    // https://raw.githubusercontent.com/chromiumembedded/cef/master/include/internal/cef_types.h
    //
    // Chromium's macOS synthetic NSEvent path reconstructs only those same
    // modifiers when CEF turns the key event back into a native event for
    // browser-side processing:
    // https://chromium.googlesource.com/chromium/src/+/refs/tags/147.0.7727.118/components/input/native_web_keyboard_event_mac.mm
    //
    // So Fn/Globe-E and a plain E collapse to the same CefKeyEvent before CEF
    // menu-key handling can see them. For a media player we do not need these
    // text-input helpers, and leaving them enabled lets a plain "e" trigger the
    // Character Palette on some macOS setups. Disable the automatic items
    // entirely as it cannot be handled easily.
    [[NSUserDefaults standardUserDefaults] setBool:YES forKey:@"NSDisabledDictationMenuItem"];
    [[NSUserDefaults standardUserDefaults] setBool:YES forKey:@"NSDisabledCharacterPaletteMenuItem"];

    // Menu bar: App (About, Quit) + Edit (standard editing shortcuts)
    g_app_menu_target = [[JellyfinAppMenuTarget alloc] init];

    NSMenu* menubar = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:appMenuItem];
    NSMenu* appMenu = [[NSMenu alloc] init];

    NSMenuItem* aboutItem =
        [[NSMenuItem alloc] initWithTitle:@"About Jellyfin Desktop"
                                   action:@selector(showAbout:)
                            keyEquivalent:@""];
    [aboutItem setTarget:g_app_menu_target];
    [appMenu addItem:aboutItem];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Hide Jellyfin Desktop"
                                                action:@selector(hide:)
                                         keyEquivalent:@"h"]];
    NSMenuItem* hideOthersItem =
        [[NSMenuItem alloc] initWithTitle:@"Hide Others"
                                   action:@selector(hideOtherApplications:)
                            keyEquivalent:@"h"];
    hideOthersItem.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [appMenu addItem:hideOthersItem];
    [appMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Show All"
                                                action:@selector(unhideAllApplications:)
                                         keyEquivalent:@""]];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Quit"
                                                action:@selector(terminate:)
                                         keyEquivalent:@"q"]];
    [appMenuItem setSubmenu:appMenu];

    // Edit menu
    NSMenuItem* editMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:editMenuItem];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Undo"
                                                 action:@selector(undo:)
                                          keyEquivalent:@"z"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Redo"
                                                 action:@selector(redo:)
                                          keyEquivalent:@"Z"]];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Cut"
                                                 action:@selector(cut:)
                                          keyEquivalent:@"x"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Copy"
                                                 action:@selector(copy:)
                                          keyEquivalent:@"c"]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Paste"
                                                 action:@selector(paste:)
                                          keyEquivalent:@"v"]];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Select All"
                                                 action:@selector(selectAll:)
                                          keyEquivalent:@"a"]];
    [editMenuItem setSubmenu:editMenu];

    [NSApp setMainMenu:menubar];

    // -[NSApp run] calls -finishLaunching internally; an explicit call here
    // is redundant and crashes -[NSCarbonMenuImpl _createMenuRef] on macOS 12.
    [NSApp activateIgnoringOtherApps:YES];
}

// macos_set_idle_inhibit now lives in src/macos/src/lib.rs.

// macos_clipboard_read_text_async + macos_open_external_url now live in
// src/macos/src/lib.rs.

// Per-surface lifecycle (alloc_surface, free_surface, surface_present,
// surface_resize, surface_set_visible, restack, fade_surface) all live
// in src/macos/src/lib.rs. Platform vtable composition is in the same
// crate (jfn-macos).
