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
#include "input/input_macos.h"
#include "logging.h"

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

static void macos_pump();

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
// Compositor state (two CAMetalLayers: main + overlay)
// =====================================================================
//
// CEF's OSR pipeline delivers a BGRA8 IOSurface in STRAIGHT alpha via
// OnAcceleratedPaint (confirmed from Chromium: components/viz/service/
// frame_sinks/video_capture/video_capture_overlay_unittest.cc:476-477
// "kUnpremul_SkAlphaType since that is the semantics of PIXEL_FORMAT_ARGB").
// CoreAnimation expects premultiplied contents, so we render CEF's
// IOSurface into a CAMetalLayer drawable with premultiplication in the
// fragment shader. CAMetalLayer.colorspace = sRGB tells CA how to
// color-manage the content for the display (P3, EDR, etc.).

static id<MTLDevice> g_mtl_device = nil;
static id<MTLCommandQueue> g_mtl_queue = nil;
static id<MTLRenderPipelineState> g_mtl_pipeline = nil;


// Per-surface state. One per CefLayer (allocated by macos_alloc_surface,
// destroyed by macos_free_surface). The bottom-most surface in the
// current stack is treated as the cef-main surface for fullscreen-
// transition gating (see g_transitioning / macos_begin_transition).
struct PlatformSurface {
    NSView* __strong view = nil;
    CAMetalLayer* __strong layer = nil;

    // Input side: CEF's IOSurface, wrapped as an MTLTexture for sampling.
    // Recreated when the input IOSurface changes (new frame may use a new
    // backing surface from CEF's own pool).
    IOSurfaceRef cached_input = nullptr;
    id<MTLTexture> __strong input_texture = nil;
};

// Current stack order, bottom-to-top, as last applied via macos_restack.
// stack[0] is the cef-main surface for transition-gating purposes.
static std::vector<PlatformSurface*> g_surface_stack;

// Input NSView (owned by input::macos)
static NSView* g_input_view = nil;

// Window + transition state
static NSWindow* g_window = nullptr;
static int g_expected_w = 0, g_expected_h = 0;
static bool g_transitioning = false;

// CADisplayLink drives CEF BeginFrame production synchronized with the
// real display refresh. The callback fires on the main runloop each
// vsync and calls SendExternalBeginFrame on each browser whose host is
// ready. CEF produces a frame immediately if its compositor has
// invalidation, or does nothing if not — no polling, no wasted work.
static CADisplayLink* g_display_link = nil;

// Metal shaders (fullscreen triangle, straight→premultiplied alpha).
// Output target is a plain BGRA8 render texture backed by our IOSurface;
// the fragment shader's `color.rgb *= color.a` converts from CEF's straight
// alpha to the premultiplied convention CoreAnimation expects.
static NSString* const g_shader_source = @R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex VertexOut vertexShader(uint vertexID [[vertex_id]]) {
    float2 positions[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };
    float2 texCoords[3] = { float2(0,1), float2(2,1), float2(0,-1) };
    VertexOut out;
    out.position = float4(positions[vertexID], 0.0, 1.0);
    out.texCoord = texCoords[vertexID];
    return out;
}

fragment float4 fragmentShader(VertexOut in [[stage_in]],
                                texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear);
    float4 color = tex.sample(s, in.texCoord);
    color.rgb *= color.a;
    return color;
}
)";

// =====================================================================
// Input surface caching
// =====================================================================

// Wrap the CEF input IOSurface as an MTLTexture for sampling. Recreated
// when the input surface identity changes. Also updates the layer's
// colorspace from the IOSurface's kIOSurfaceColorSpace tag (set by
// Chromium's viz compositor). Falls back to sRGB if untagged.
static bool wrap_input_surface(PlatformSurface& s, IOSurfaceRef surface, int w, int h) {
    if (surface == s.cached_input && s.input_texture != nil) return true;

    MTLTextureDescriptor* desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:w height:h mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [g_mtl_device newTextureWithDescriptor:desc
                                                       iosurface:surface
                                                           plane:0];
    if (!tex) {
        LOG_ERROR(LOG_PLATFORM, "[METAL] wrap input IOSurface failed");
        return false;
    }
    s.input_texture = tex;
    s.cached_input = surface;

    CFTypeRef cs = IOSurfaceCopyValue(surface, kIOSurfaceColorSpace);
    if (cs && CFGetTypeID(cs) == CFStringGetTypeID()) {
        CGColorSpaceRef cg = CGColorSpaceCreateWithName((CFStringRef)cs);
        if (cg) {
            s.layer.colorspace = cg;
            CGColorSpaceRelease(cg);
        }
    }
    if (cs) CFRelease(cs);

    return true;
}

// =====================================================================
// Present helper: render CEF's straight-alpha IOSurface into the
// CAMetalLayer's next drawable with premultiplied alpha.
// =====================================================================

static void present_iosurface(PlatformSurface& s, const CefAcceleratedPaintInfo& info) {
    if (!g_mtl_device || !s.layer) {
        LOG_WARN(LOG_PLATFORM, "[METAL] present skipped: device={} layer={}",
                 (__bridge void*)g_mtl_device, (__bridge void*)s.layer);
        return;
    }

    IOSurfaceRef surface = (IOSurfaceRef)info.shared_texture_io_surface;
    if (!surface) {
        LOG_WARN(LOG_PLATFORM, "[METAL] present skipped: null IOSurface");
        return;
    }

    int w = IOSurfaceGetWidth(surface);
    int h = IOSurfaceGetHeight(surface);

    if (!wrap_input_surface(s, surface, w, h)) return;

    // Update drawable size if the input dimensions changed.
    CGSize cur = s.layer.drawableSize;
    if ((int)cur.width != w || (int)cur.height != h) {
        s.layer.drawableSize = CGSizeMake(w, h);
    }

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [s.layer nextDrawable];
        if (!drawable) {
            LOG_WARN(LOG_PLATFORM, "[METAL] nextDrawable returned nil");
            return;
        }

        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        passDesc.colorAttachments[0].texture     = drawable.texture;
        passDesc.colorAttachments[0].loadAction  = MTLLoadActionClear;
        passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
        passDesc.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 0);

        id<MTLCommandBuffer> cmdBuf = [g_mtl_queue commandBuffer];
        id<MTLRenderCommandEncoder> enc =
            [cmdBuf renderCommandEncoderWithDescriptor:passDesc];
        [enc setRenderPipelineState:g_mtl_pipeline];
        [enc setFragmentTexture:s.input_texture atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cmdBuf presentDrawable:drawable];
        [cmdBuf commit];
    }
}

// =====================================================================
// Helper: create a CAMetalLayer + hosting NSView
// =====================================================================

static void create_content_layer(NSView* contentView, CGRect frame, CGFloat scale,
                                 NSView* __strong& out_view, CAMetalLayer* __strong& out_layer,
                                 NSView* positionAbove) {
    out_view = [[NSView alloc] initWithFrame:frame];
    [out_view setWantsLayer:YES];
    [out_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    out_layer = [CAMetalLayer layer];
    out_layer.device = g_mtl_device;
    out_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    out_layer.colorspace = srgb;
    CGColorSpaceRelease(srgb);
    out_layer.framebufferOnly = YES;
    out_layer.frame = frame;
    out_layer.contentsScale = scale;
    out_layer.opaque = NO;
    // Disable implicit animations on property changes — we update contents
    // every frame and don't want CA to cross-fade between them.
    out_layer.actions = @{
        @"bounds":       [NSNull null],
        @"position":     [NSNull null],
        @"contents":     [NSNull null],
        @"anchorPoint":  [NSNull null],
        @"contentsRect": [NSNull null],
    };

    [out_view setLayer:out_layer];
    [contentView addSubview:out_view positioned:NSWindowAbove relativeTo:positionAbove];
}

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
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (g_browsers) {
        g_browsers->forEachBrowser([](CefRefPtr<CefBrowser> b) {
            b->GetHost()->SendExternalBeginFrame();
        });
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

static void macos_set_theme_color(const Color& c) {
    // Updates AppKit fills behind mpv's CAMetalLayer / NSWindow root so the
    // resize-gap stale-texture window (which CLAUDE.md explicitly accepts
    // over stretching) matches mpv's own background — no flash visible.
    NSColor* ns = [NSColor colorWithSRGBRed:c.r/255.0 green:c.g/255.0
                                       blue:c.b/255.0 alpha:1.0];
    auto apply = ^{
        if (!g_window) return;
        g_window.backgroundColor = ns;
        if (NSView* cv = [g_window contentView]; cv.layer)
            cv.layer.backgroundColor = ns.CGColor;
    };
    if ([NSThread isMainThread]) apply();
    else dispatch_async(dispatch_get_main_queue(), apply);
}

static bool macos_init(mpv_handle* mpv) {
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
    g_mpv.SetForceWindowPosition(false);

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
    macos_set_theme_color(kBgColor);

    // Metal setup
    g_mtl_device = MTLCreateSystemDefaultDevice();
    if (!g_mtl_device) { fprintf(stderr, "Metal device creation failed\n"); return false; }
    g_mtl_queue = [g_mtl_device newCommandQueue];

    NSError* error = nil;
    id<MTLLibrary> library = [g_mtl_device newLibraryWithSource:g_shader_source options:nil error:&error];
    if (!library) { fprintf(stderr, "Metal shader compile: %s\n", [[error localizedDescription] UTF8String]); return false; }

    // Render pipeline: writes straight → premultiplied conversion into a
    // plain BGRA8 render target (no blending; the render target is our
    // own IOSurface and we overwrite the whole thing each frame).
    MTLRenderPipelineDescriptor* pipeDesc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeDesc.vertexFunction = [library newFunctionWithName:@"vertexShader"];
    pipeDesc.fragmentFunction = [library newFunctionWithName:@"fragmentShader"];
    pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeDesc.colorAttachments[0].blendingEnabled = NO;
    g_mtl_pipeline = [g_mtl_device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
    if (!g_mtl_pipeline) { fprintf(stderr, "Metal pipeline: %s\n", [[error localizedDescription] UTF8String]); return false; }

    // CefLayer surfaces are created on demand via macos_alloc_surface and
    // ordered by macos_restack. The input NSView sits above whatever
    // CefLayer subviews currently exist; macos_restack re-anchors it on
    // top after any reorder.
    CGRect frame = [contentView bounds];

    g_input_view = input::macos::create_input_view();
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

// True when s is the bottom-most surface in the current stack (cef-main).
// Used to gate fullscreen-transition logic onto the main surface only.
static bool is_cef_main(PlatformSurface* s) {
    return !g_surface_stack.empty() && g_surface_stack.front() == s;
}

// =====================================================================
// Native NSMenu popup (replaces CEF's HTML popup widget for <select>)
// =====================================================================
//
// CEF's Alloy OSR popup widget renders hover/selection highlights as
// opaque black on macOS (compositor-level issue we can't reach). Instead
// we let CEF's popup widget run invisibly in the background and display
// a native NSMenu in its place. On selection, we send the chosen index
// back to the renderer process for application, then send Escape to
// dismiss CEF's internal popup widget (which fires OnPopupShow(false)).

@interface JellyfinPopupMenuTarget : NSObject {
@public
    std::function<void(int)> on_selected;
    BOOL fired;
}
- (void)itemPicked:(NSMenuItem*)item;
- (void)fireCancelIfNeeded;
@end

@implementation JellyfinPopupMenuTarget
- (void)itemPicked:(NSMenuItem*)item {
    if (fired) return;
    fired = YES;
    if (on_selected) on_selected((int)[item tag]);
}
- (void)fireCancelIfNeeded {
    if (fired) return;
    fired = YES;
    if (on_selected) on_selected(-1);
}
@end

static void macos_popup_show(PlatformSurface*, const Platform::PopupRequest& req) {
    // NSMenu is a window-level OS overlay — not tied to a CefLayer
    // surface, so the surface arg is ignored.
    if (!g_window || !g_input_view || req.options.empty()) return;

    auto opts = req.options;
    int cur = req.initial_highlight;
    int px = req.x, py = req.y, plw = req.lw;
    auto cb = std::make_shared<std::function<void(int)>>(req.on_selected);

    dispatch_async(dispatch_get_main_queue(), ^{
        NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
        [menu setAutoenablesItems:NO];

        JellyfinPopupMenuTarget* target = [[JellyfinPopupMenuTarget alloc] init];
        target->on_selected = [cb](int idx) { if (*cb) (*cb)(idx); };
        target->fired = NO;

        for (size_t i = 0; i < opts.size(); i++) {
            NSString* title = [NSString stringWithUTF8String:opts[i].c_str()];
            NSMenuItem* item =
                [[NSMenuItem alloc] initWithTitle:title
                                           action:@selector(itemPicked:)
                                    keyEquivalent:@""];
            [item setTag:(NSInteger)i];
            [item setTarget:target];
            if ((int)i == cur) [item setState:NSControlStateValueOn];
            [menu addItem:item];
        }

        // Anchor in g_input_view — it's isFlipped=YES so (x, y) map directly
        // without a contentView-height subtraction.
        NSPoint location = NSMakePoint((CGFloat)px, (CGFloat)py);
        NSMenuItem* initial = (cur >= 0 && cur < (int)opts.size())
            ? [menu itemAtIndex:cur] : nil;
        [menu setMinimumWidth:(CGFloat)plw];

        [menu popUpMenuPositioningItem:initial
                            atLocation:location
                                inView:g_input_view];
        // popUpMenuPositioningItem is modal; if no item was picked, cancel.
        [target fireCancelIfNeeded];
    });
}

// Per-surface visibility. Focus management is owned by Browsers::setActive,
// not by the platform — matches the Wayland and Windows backends.
static void macos_surface_set_visible(PlatformSurface* s, bool visible) {
    if (!s) return;
    auto apply = ^{
        if (s->view) [s->view setHidden:!visible];
    };
    if ([NSThread isMainThread]) apply();
    else dispatch_async(dispatch_get_main_queue(), apply);
}

// Per-surface opacity fade. Mirrors the previous overlay-only fade path:
// animate CALayer.opacity from 1.0 to 0.0 over fade_sec, fire on_fade_start
// before kicking the animation, fire on_complete from CATransaction's
// completion block. After the animation we hide the view and reset opacity
// so a subsequent setVisible(true) shows it fully opaque again.
static void macos_fade_surface(PlatformSurface* s, float fade_sec,
                               std::function<void()> on_fade_start,
                               std::function<void()> on_complete) {
    if (!s || !s->view || !s->view.layer) {
        if (on_fade_start) on_fade_start();
        macos_surface_set_visible(s, false);
        if (on_complete) on_complete();
        return;
    }
    // Copy into block-friendly shared_ptrs so the callbacks survive into the block chain.
    auto start_cb = std::make_shared<std::function<void()>>(std::move(on_fade_start));
    auto done_cb = std::make_shared<std::function<void()>>(std::move(on_complete));
    PlatformSurface* surface_ptr = s;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (*start_cb) (*start_cb)();
        if (!surface_ptr->view || !surface_ptr->view.layer) {
            if (*done_cb) (*done_cb)();
            return;
        }
        CABasicAnimation* fade = [CABasicAnimation animationWithKeyPath:@"opacity"];
        fade.fromValue = @1.0;
        fade.toValue = @0.0;
        fade.duration = fade_sec;
        fade.removedOnCompletion = NO;
        fade.fillMode = kCAFillModeForwards;
        [CATransaction begin];
        [CATransaction setCompletionBlock:^{
            if (surface_ptr->view) [surface_ptr->view setHidden:YES];
            if (surface_ptr->view.layer) {
                [surface_ptr->view.layer removeAllAnimations];
                surface_ptr->view.layer.opacity = 1.0;
            }
            if (*done_cb) (*done_cb)();
        }];
        [surface_ptr->view.layer addAnimation:fade forKey:@"fadeOut"];
        [CATransaction commit];
    });
}

static void macos_set_fullscreen(bool fullscreen) {
    if (!g_mpv.IsValid()) return;
    g_mpv.SetFullscreen(fullscreen);
}

static void macos_toggle_fullscreen() {
    if (!g_mpv.IsValid()) return;
    g_mpv.ToggleFullscreen();
}

static void macos_begin_transition() {
    g_transitioning = true;
    // Drop cached input-surface wrappers across the whole stack so the
    // next paint re-wraps at the new size. drawableSize is updated in
    // present_iosurface.
    for (PlatformSurface* s : g_surface_stack) {
        s->input_texture = nil;
        s->cached_input = nullptr;
    }
}

static void macos_end_transition() {}

static bool macos_in_transition() { return g_transitioning; }

static void macos_set_expected_size(int w, int h) {
    g_expected_w = w;
    g_expected_h = h;
}

static float macos_get_scale() {
    if (g_window) return static_cast<float>([g_window backingScaleFactor]);
    // Pre-window: fall back to the main screen's scale so callers (e.g.
    // default-geometry sizing at startup) get an accurate value.
    NSScreen* screen = [NSScreen mainScreen];
    if (screen) return static_cast<float>([screen backingScaleFactor]);
    return 1.0f;
}

namespace macos_platform {
bool query_logical_content_size(int* w, int* h) {
    if (!g_window) return false;
    NSRect bounds = [[g_window contentView] bounds];
    *w = static_cast<int>(bounds.size.width);
    *h = static_cast<int>(bounds.size.height);
    return *w > 0 && *h > 0;
}
}

static bool macos_query_window_position(int* x, int* y) {
    if (!g_window || ![g_window screen]) return false;
    // mpv's --geometry +X+Y is in backing pixels, relative to the screen's
    // visible frame (excluding menu bar / dock), with Y measured from the
    // top. Match that coordinate system exactly so save→restore is lossless.
    NSScreen* screen = [g_window screen];
    NSRect frame = [g_window frame];
    NSRect visible = [screen visibleFrame];
    CGFloat scale = [screen backingScaleFactor];
    // Logical offset within the visible frame, then convert to backing pixels.
    CGFloat lx = frame.origin.x - visible.origin.x;
    CGFloat ly = (visible.origin.y + visible.size.height)
               - (frame.origin.y + frame.size.height);
    *x = static_cast<int>(lx * scale);
    *y = static_cast<int>(ly * scale);
    return true;
}

static void macos_clamp_window_geometry(int* w, int* h, int* x, int* y) {
    NSScreen* screen = [NSScreen mainScreen];
    if (!screen) return;
    NSRect visible = [screen visibleFrame];
    CGFloat scale = [screen backingScaleFactor];
    int vw = static_cast<int>(visible.size.width * scale);
    int vh = static_cast<int>(visible.size.height * scale);
    // Shrink to fit
    if (*w > vw) *w = vw;
    if (*h > vh) *h = vh;
    // Center any unset axis (mpv's own centering misbehaves when we override
    // --geometry's wh but leave xy unset: it pre-centers against the video
    // size and doesn't re-center after applying the requested wh).
    if (*x < 0) *x = (vw - *w) / 2;
    if (*y < 0) *y = (vh - *h) / 2;
    // Clamp saved position so the window stays fully on-screen
    if (*x + *w > vw) *x = vw - *w;
    if (*y + *h > vh) *y = vh - *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

static void macos_pump() {
    @autoreleasepool {
        // distantPast = return immediately if no event. `nil` means distantFuture
        // (block forever), which would freeze the caller. Used during the
        // pre-CefInitialize wait-for-VO loop where we interleave with mpv events.
        NSEvent* event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate distantPast]
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES])) {
            [NSApp sendEvent:event];
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
    }
}

// Block on the NSApplication run loop. Returns when wake_main_loop calls
// [NSApp stop:nil]. [NSApp run] is the canonical Cocoa main loop and
// properly services every run-loop mode CEF and AppKit care about (default,
// event-tracking during drag, modal panels, etc.) — which a hand-rolled
// nextEventMatchingMask loop in NSDefaultRunLoopMode does not. CFRunLoop
// sources installed in CommonModes (like the CEF wake source set up in
// cef_app.cpp:InitWakePipe) and GCD main-queue blocks (mpv VO
// DispatchQueue.main.sync) all fire from inside this call without polling.
// Mirrors cefclient's MainMessageLoopExternalPumpMac::Run.
static void macos_run_main_loop() {
    LOG_INFO(LOG_PLATFORM, "[NSAPP] macos_run_main_loop: entering [NSApp run]");
    [NSApp run];
    LOG_INFO(LOG_PLATFORM, "[NSAPP] macos_run_main_loop: [NSApp run] returned");
}

// Stop the NSApplication run loop from any thread. dispatch_async hops to
// main and runs the block from inside [NSApp run]'s servicing of CFRunLoop;
// the block then calls -stop: which marks the loop for exit on its next
// iteration. A sentinel applicationDefined NSEvent guarantees there *is* a
// next iteration even if no other events arrive. Documented thread-safe.
static void macos_wake_main_loop() {
    LOG_INFO(LOG_PLATFORM, "[NSAPP] macos_wake_main_loop: requesting stop");
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            LOG_INFO(LOG_PLATFORM, "[NSAPP] macos_wake_main_loop: [NSApp stop:] on main");
            [NSApp stop:nil];
            NSEvent* sentinel = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                   location:NSZeroPoint
                                              modifierFlags:0
                                                  timestamp:0
                                               windowNumber:0
                                                    context:nil
                                                    subtype:0
                                                      data1:0
                                                      data2:0];
            [NSApp postEvent:sentinel atStart:YES];
        }
    });
}

static void macos_cleanup() {
    // Stop the display link first so no more BeginFrames race the teardown.
    stop_display_link();

    if (g_input_view) { [g_input_view removeFromSuperview]; g_input_view = nil; }

    // Browsers::~Browsers should have called free_surface on each layer,
    // but defensively tear down any stragglers so AppKit objects are
    // released before the window goes away.
    for (PlatformSurface* s : g_surface_stack) {
        if (s->view) [s->view removeFromSuperview];
        s->view = nil;
        s->layer = nil;
        s->input_texture = nil;
        s->cached_input = nullptr;
    }
    g_surface_stack.clear();

    g_mtl_pipeline = nil; g_mtl_queue = nil; g_mtl_device = nil;
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

static void macos_early_init() {
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

static IOPMAssertionID g_idle_assertion = kIOPMNullAssertionID;

static void macos_set_idle_inhibit(IdleInhibitLevel level) {
    // Release existing assertion if one is active
    if (g_idle_assertion != kIOPMNullAssertionID) {
        IOPMAssertionRelease(g_idle_assertion);
        g_idle_assertion = kIOPMNullAssertionID;
    }

    // If level is None, just return after releasing
    if (level == IdleInhibitLevel::None) {
        return;
    }

    // Determine assertion type based on level
    CFStringRef type = nullptr;
    if (level == IdleInhibitLevel::Display) {
        type = kIOPMAssertionTypePreventUserIdleDisplaySleep;
    } else if (level == IdleInhibitLevel::System) {
        type = kIOPMAssertionTypePreventUserIdleSystemSleep;
    }

    if (type) {
        IOPMAssertionCreateWithName(type, kIOPMAssertionLevelOn,
                                    CFSTR("Jellyfin Desktop media playback"),
                                    &g_idle_assertion);
    }
}

// =====================================================================
// Clipboard (NSPasteboard) — read only; writes go through CEF's own
// frame->Copy() path which works correctly on macOS.
// =====================================================================

static void macos_clipboard_read_text_async(std::function<void(std::string)> on_done) {
    if (!on_done) return;
    // NSPasteboard reads are synchronous on macOS; fire the callback inline.
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    NSString* ns = [pb stringForType:NSPasteboardTypeString];
    const char* utf8 = ns ? [ns UTF8String] : nullptr;
    on_done(utf8 ? std::string(utf8) : std::string());
}

static void macos_open_external_url(const std::string& url) {
    NSString* str = [NSString stringWithUTF8String:url.c_str()];
    NSURL* nsurl = str ? [NSURL URLWithString:str] : nil;
    if (!nsurl) {
        LOG_ERROR(LOG_PLATFORM, "open_external_url: invalid URL: {}", url);
        return;
    }
    if (![[NSWorkspace sharedWorkspace] openURL:nsurl])
        LOG_ERROR(LOG_PLATFORM, "NSWorkspace openURL failed: {}", url);
}

// =====================================================================
// Generic per-surface lifecycle / present / resize / restack
// =====================================================================

// All AppKit operations must run on the main thread; if Browsers calls
// alloc/free/restack/resize off-main, dispatch_sync onto the main queue.
template <typename Block>
static void run_on_main_sync(Block block) {
    if ([NSThread isMainThread]) { block(); return; }
    dispatch_sync(dispatch_get_main_queue(), block);
}

static PlatformSurface* macos_alloc_surface() {
    auto* s = new PlatformSurface;
    run_on_main_sync(^{
        if (!g_window) return;
        NSView* contentView = [g_window contentView];
        if (!contentView) return;
        CGRect frame = [contentView bounds];
        CGFloat scale = [g_window backingScaleFactor];
        // positionAbove=nil — final ordering is applied by macos_restack
        // once Browsers pushes the new layer onto the stack.
        create_content_layer(contentView, frame, scale, s->view, s->layer, nil);
    });
    return s;
}

static void macos_free_surface(PlatformSurface* s) {
    if (!s) return;
    run_on_main_sync(^{
        // Defensive remove from the cached stack — Browsers will normally
        // restack to a smaller order after this call, but clearing the
        // entry here keeps is_cef_main coherent in the meantime.
        auto it = std::find(g_surface_stack.begin(), g_surface_stack.end(), s);
        if (it != g_surface_stack.end()) g_surface_stack.erase(it);
        if (s->view) [s->view removeFromSuperview];
        s->view = nil;
        s->layer = nil;
        s->input_texture = nil;
        s->cached_input = nullptr;
    });
    delete s;
}

static void macos_surface_present(PlatformSurface* s, const CefAcceleratedPaintInfo& info) {
    if (!s) return;
    // Fullscreen-transition gating runs only on the cef-main surface
    // (bottom of stack), matching the pre-refactor macos_present path.
    if (is_cef_main(s)) {
        if (g_transitioning) return;
        present_iosurface(*s, info);
        if (g_expected_w > 0) {
            IOSurfaceRef surface = (IOSurfaceRef)info.shared_texture_io_surface;
            if (surface && (int)IOSurfaceGetWidth(surface) == g_expected_w &&
                (int)IOSurfaceGetHeight(surface) == g_expected_h) {
                g_expected_w = 0; g_expected_h = 0;
                g_transitioning = false;
            }
        }
        return;
    }
    present_iosurface(*s, info);
}

static void macos_surface_present_software(PlatformSurface*,
                                           const CefRenderHandler::RectList&,
                                           const void*, int, int) {
    // CEF on macOS runs hardware-accelerated (shared_texture_supported=true);
    // the software path is not exercised. Kept for API completeness.
}

static void macos_surface_resize(PlatformSurface* s, int lw, int lh, int pw, int ph) {
    if (!s) return;
    // The NSView is autoresized to fit the contentView, and present_iosurface
    // updates the CAMetalLayer.drawableSize when CEF delivers a frame at the
    // new pixel size. Update contentsScale + drawableSize defensively so
    // resizes that don't immediately produce a new CEF frame still take
    // effect on the layer.
    auto apply = ^{
        if (!s->view || !s->layer) return;
        // Setting the NSView frame is redundant under autoresizing but
        // keeps the layer geometry in sync for late configures.
        s->view.frame = [[g_window contentView] bounds];
        CGFloat scale = pw > 0 && lw > 0 ? (CGFloat)pw / (CGFloat)lw
                                         : [g_window backingScaleFactor];
        s->layer.contentsScale = scale;
        if (pw > 0 && ph > 0)
            s->layer.drawableSize = CGSizeMake(pw, ph);
        (void)lh;
    };
    if ([NSThread isMainThread]) apply();
    else dispatch_async(dispatch_get_main_queue(), apply);
}

static void macos_restack(PlatformSurface* const* ordered, size_t n) {
    auto apply = ^{
        g_surface_stack.assign(ordered, ordered + n);
        if (!g_window) return;
        NSView* contentView = [g_window contentView];
        if (!contentView) return;
        NSView* prev = nil;
        for (size_t i = 0; i < n; i++) {
            PlatformSurface* s = ordered[i];
            if (!s || !s->view) continue;
            // addSubview:positioned:relativeTo: re-anchors an existing
            // subview; safe to call repeatedly.
            [contentView addSubview:s->view positioned:NSWindowAbove relativeTo:prev];
            prev = s->view;
        }
        // Keep the input view on top of every CefLayer.
        if (g_input_view)
            [contentView addSubview:g_input_view positioned:NSWindowAbove relativeTo:prev];
    };
    if ([NSThread isMainThread]) apply();
    else dispatch_sync(dispatch_get_main_queue(), apply);
}

Platform make_macos_platform() {
    return Platform{
        .display = DisplayBackend::macOS,
        .early_init = macos_early_init,
        .init = macos_init,
        .cleanup = macos_cleanup,
        .post_window_cleanup = nullptr,
        .alloc_surface = macos_alloc_surface,
        .free_surface = macos_free_surface,
        .surface_present = macos_surface_present,
        .surface_present_software = macos_surface_present_software,
        .surface_resize = macos_surface_resize,
        .surface_set_visible = macos_surface_set_visible,
        .restack = macos_restack,
        .fade_surface = macos_fade_surface,
        // macos_popup_show substitutes a native NSMenu for CEF's popup
        // widget (which renders highlights as opaque black on macOS).
        // popup_present[_software] / popup_hide are no-ops — NSMenu owns
        // its own pixels and lifecycle.
        .popup_show = macos_popup_show,
        .popup_hide = [](PlatformSurface*) {},
        .popup_present = [](PlatformSurface*, const CefAcceleratedPaintInfo&, int, int) {},
        .popup_present_software = [](PlatformSurface*, const void*, int, int, int, int) {},
        .set_fullscreen = macos_set_fullscreen,
        .toggle_fullscreen = macos_toggle_fullscreen,
        .begin_transition = macos_begin_transition,
        .end_transition = macos_end_transition,
        .in_transition = macos_in_transition,
        .set_expected_size = macos_set_expected_size,
        .get_scale = macos_get_scale,
        .query_window_position = macos_query_window_position,
        .clamp_window_geometry = macos_clamp_window_geometry,
        .pump = macos_pump,
        .run_main_loop = macos_run_main_loop,
        .wake_main_loop = macos_wake_main_loop,
        .set_cursor = input::macos::set_cursor,
        .set_idle_inhibit = macos_set_idle_inhibit,
        .set_theme_color = macos_set_theme_color,
        .clipboard_read_text_async = macos_clipboard_read_text_async,
        .open_external_url = macos_open_external_url,
    };
}
