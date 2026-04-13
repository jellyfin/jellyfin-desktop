// platform_macos.mm — macOS platform layer.
// Two plain CALayers composite CEF IOSurfaces (main + overlay) onto mpv's
// window. CEF delivers straight-alpha BGRA via OnAcceleratedPaint; a Metal
// pass converts to premultiplied into a small IOSurface pool we own, then
// assigns that IOSurface as layer.contents — CoreAnimation handles the
// actual compositing on its render-server thread. No CAMetalLayer, no
// nextDrawable, no VSync-bound blocking on the main thread.
// Input is owned by src/input/input_macos.mm.

#include "platform/platform.h"
#include "common.h"
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
#include <mach/mach_time.h>
#include <objc/runtime.h>

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
// Compositor state (two CALayers: main + overlay)
// =====================================================================
//
// CEF's OSR pipeline delivers a BGRA8 IOSurface in STRAIGHT alpha via
// OnAcceleratedPaint (confirmed from Chromium: components/viz/service/
// frame_sinks/video_capture/video_capture_overlay_unittest.cc:476-477
// "kUnpremul_SkAlphaType since that is the semantics of PIXEL_FORMAT_ARGB").
// CoreAnimation expects premultiplied contents, so we can't use the CEF
// IOSurface as layer.contents directly — CoreAnimation would composite the
// edges too bright.
//
// Design:
//
//   [CEF IOSurface, straight alpha]
//       │ MTLTexture wrap (read)
//       ▼
//   Metal render pass (premultiply in fragment shader)
//       │ writes to
//       ▼
//   [Our IOSurface, premultiplied alpha]  ← pool of 3, round-robin
//       │ command buffer completion handler
//       │ dispatches to main queue
//       ▼
//   layer.contents = (id)ourIOSurface; [CATransaction commit];
//       │
//       ▼
//   CoreAnimation composites on its own render server thread.
//
// This is the macOS analogue of the Wayland dmabuf→wl_surface.attach path.
// The Metal pass is fast (sub-millisecond to encode+commit), non-blocking,
// and runs the GPU work on Metal's private queue. The main thread's
// OnAcceleratedPaint returns before CoreAnimation ever touches the result.
//
// A pool of 3 IOSurfaces per layer provides safe rotation: when we assign
// surface N to the layer, surface N-2 (the one we'll overwrite next) is
// two frames old — well past the point where the render server could still
// be reading it.

static id<MTLDevice> g_mtl_device = nil;
static id<MTLCommandQueue> g_mtl_queue = nil;
static id<MTLRenderPipelineState> g_mtl_pipeline = nil;

constexpr int kPoolSize = 3;

struct LayerState {
    NSView* __strong view;
    CALayer* __strong layer;

    // Input side: CEF's IOSurface, wrapped as an MTLTexture for sampling.
    // Recreated when the input IOSurface changes (new frame may use a new
    // backing surface from CEF's own pool).
    IOSurfaceRef cached_input;
    id<MTLTexture> __strong input_texture;

    // Output side: our premultiplied IOSurfaces + render-target MTLTexture
    // wrappers. All sized pool_w × pool_h in physical pixels.
    IOSurfaceRef pool[kPoolSize];
    id<MTLTexture> __strong pool_textures[kPoolSize];
    int pool_w;
    int pool_h;
    int next_write;  // round-robin index into pool[]
};

static LayerState g_main{};
static LayerState g_overlay{};
static bool g_overlay_visible = false;

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
// IOSurface pool management
// =====================================================================

static IOSurfaceRef create_premul_iosurface(int w, int h) {
    NSDictionary* props = @{
        (__bridge NSString*)kIOSurfaceWidth:           @(w),
        (__bridge NSString*)kIOSurfaceHeight:          @(h),
        (__bridge NSString*)kIOSurfaceBytesPerElement: @(4),
        (__bridge NSString*)kIOSurfacePixelFormat:     @((uint32_t)'BGRA'),
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

static void release_pool(LayerState& s) {
    for (int i = 0; i < kPoolSize; i++) {
        s.pool_textures[i] = nil;
        if (s.pool[i]) {
            CFRelease(s.pool[i]);
            s.pool[i] = nullptr;
        }
    }
    s.pool_w = 0;
    s.pool_h = 0;
    s.next_write = 0;
}

// Ensure the output IOSurface pool matches the input pixel dimensions.
// Recreates everything if the size changed. Returns true if the pool is
// ready to use.
static bool ensure_pool(LayerState& s, int w, int h) {
    if (s.pool_w == w && s.pool_h == h && s.pool[0] != nullptr) return true;

    LOG_INFO(LOG_PLATFORM, "[POOL] resize %dx%d -> %dx%d", s.pool_w, s.pool_h, w, h);
    release_pool(s);

    for (int i = 0; i < kPoolSize; i++) {
        s.pool[i] = create_premul_iosurface(w, h);
        if (!s.pool[i]) {
            LOG_ERROR(LOG_PLATFORM, "[POOL] IOSurfaceCreate failed i=%d %dx%d", i, w, h);
            release_pool(s);
            return false;
        }
        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:w height:h mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        s.pool_textures[i] = [g_mtl_device newTextureWithDescriptor:desc
                                                            iosurface:s.pool[i]
                                                                plane:0];
        if (!s.pool_textures[i]) {
            LOG_ERROR(LOG_PLATFORM, "[POOL] newTextureWithDescriptor:iosurface: failed i=%d", i);
            release_pool(s);
            return false;
        }
    }
    s.pool_w = w;
    s.pool_h = h;
    s.next_write = 0;
    return true;
}

// Wrap the CEF input IOSurface as an MTLTexture for sampling. Recreated
// when the input surface identity changes.
static bool wrap_input_surface(LayerState& s, IOSurfaceRef surface, int w, int h) {
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
    return true;
}

// =====================================================================
// Present helper: encode the straight→premultiplied conversion pass to
// one of our pool IOSurfaces, then asynchronously assign it as the
// layer's contents once the GPU write is complete.
// =====================================================================

static void present_iosurface(LayerState& s, const CefAcceleratedPaintInfo& info) {
    if (!g_mtl_device || !s.layer) {
        LOG_WARN(LOG_PLATFORM, "[METAL] present skipped: device=%p layer=%p",
                 g_mtl_device, s.layer);
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
    if (!ensure_pool(s, w, h)) return;

    // Pick the next pool slot. Round-robin through kPoolSize buffers so the
    // slot we write to is always at least (kPoolSize-1) frames away from
    // whatever the render server might still be reading.
    int slot = s.next_write;
    s.next_write = (s.next_write + 1) % kPoolSize;

    IOSurfaceRef out_surface = s.pool[slot];
    id<MTLTexture> out_texture = s.pool_textures[slot];
    CALayer* target_layer = s.layer;

    @autoreleasepool {
        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        passDesc.colorAttachments[0].texture     = out_texture;
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
        [cmdBuf commit];

        // Assign the output IOSurface as the layer's contents synchronously,
        // right after commit. The GPU write may not be finished yet, but
        // Metal tracks IOSurface hazards across GPU contexts: when
        // WindowServer's render process reads this surface (at the next
        // CoreAnimation tick), Metal stalls the read until our write is
        // complete. No explicit fence / completion handler / main-queue hop
        // needed, and typing latency drops by the cost of those hops.
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        target_layer.contents = (__bridge id)out_surface;
        [CATransaction commit];
    }
}

// =====================================================================
// Helper: create a plain CALayer + hosting NSView
// =====================================================================

static void create_content_layer(NSView* contentView, CGRect frame, CGFloat scale,
                                 NSView* __strong& out_view, CALayer* __strong& out_layer,
                                 NSView* positionAbove) {
    out_view = [[NSView alloc] initWithFrame:frame];
    [out_view setWantsLayer:YES];
    [out_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    out_layer = [CALayer layer];
    out_layer.frame = frame;
    out_layer.contentsScale = scale;
    out_layer.opaque = NO;
    // 1:1 pixel mapping: display the contents IOSurface at its native size,
    // anchored at the top-left. Gaps during resize (when our IOSurface is
    // smaller than the layer) are acceptable; stretching is not.
    // See CLAUDE.md: "No texture stretching during resize".
    out_layer.contentsGravity = kCAGravityTopLeft;
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

extern CefRefPtr<Client>        g_client;
extern CefRefPtr<OverlayClient> g_overlay_client;
// CADisplayLink target — fires on the main runloop at the display's
// refresh rate, driving CEF's external BeginFrame production.
@interface DisplayLinkTarget : NSObject
- (void)tick:(CADisplayLink*)link;
@end

@implementation DisplayLinkTarget
- (void)tick:(CADisplayLink*)link {
    (void)link;
    if (g_shutting_down.load(std::memory_order_relaxed)) return;
    if (g_client) {
        CefRefPtr<CefBrowser> b = g_client->browser();
        if (b) b->GetHost()->SendExternalBeginFrame();
    }
    if (g_overlay_client) {
        CefRefPtr<CefBrowser> b = g_overlay_client->browser();
        if (b) b->GetHost()->SendExternalBeginFrame();
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
    LOG_INFO(LOG_PLATFORM, "[INIT] macos_init: got window=%p", g_window);

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

    // Paint a solid dark fill at every layer we can reach so the user
    // never sees uninitialized CAMetalLayer drawable content during the
    // gap between mpv creating the window and CEF delivering the first
    // overlay frame. Three levels of coverage:
    //   1. NSWindow.backgroundColor  — shows through anywhere the content-
    //      View ends up transparent.
    //   2. contentView.layer.backgroundColor — this is mpv's MetalLayer;
    //      mpv only resets it from its `isOpaque` setter (metal_layer.swift:
    //      87), which doesn't fire during normal operation, so the override
    //      sticks until a real frame is drawn on top of it.
    //   3. g_main / g_overlay layer backgroundColor — our own subview
    //      layers, set below, so that even while their CEF contents are
    //      still nil they fill with the startup color instead of exposing whatever
    //      is under them.
    NSColor* startup_bg = [NSColor colorWithSRGBRed:0x10/255.0
                                              green:0x10/255.0
                                               blue:0x10/255.0
                                              alpha:1.0];
    g_window.backgroundColor = startup_bg;
    contentView.layer.backgroundColor = startup_bg.CGColor;

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

    // Create layers: main (bottom) → overlay (middle) → input (top)
    CGRect frame = [contentView bounds];
    CGFloat scale = [g_window backingScaleFactor];

    create_content_layer(contentView, frame, scale, g_main.view, g_main.layer, nil);
    create_content_layer(contentView, frame, scale, g_overlay.view, g_overlay.layer, g_main.view);
    // Both CEF content views start hidden so nothing from their fresh
    // (empty) CALayer sublayers can leak stale window-server snapshot
    // content before CEF has actually painted. The user sees mpv's
    // contentView backing (which we've set to the startup color) until the
    // first-frame paths unhide these below. macos_overlay_present
    // unhides both in normal mode (overlay covers main as they appear
    // together); macos_present unhides g_main in player mode.
    [g_main.view setHidden:YES];
    [g_overlay.view setHidden:YES];

    g_input_view = input::macos::create_input_view();
    g_input_view.frame = contentView.bounds;
    g_input_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [contentView addSubview:g_input_view positioned:NSWindowAbove relativeTo:g_overlay.view];

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
        LOG_INFO(LOG_PLATFORM, "[WINDOW] NSWindowDidResizeNotification contentView=%.0fx%.0f",
                 b.size.width, b.size.height);
    }];

    // Start the display link. This drives CEF BeginFrame production at
    // the display's real refresh rate; without it (external_begin_frame
    // = true but no caller) CEF produces no frames at all.
    if (!start_display_link()) {
        LOG_ERROR(LOG_PLATFORM, "[INIT] failed to start CADisplayLink");
        return false;
    }

    LOG_INFO(LOG_PLATFORM, "[INIT] Metal compositor initialized (2 layers) frame=%.0fx%.0f scale=%.2f window.firstResponder=%p input_view=%p",
             frame.size.width, frame.size.height, scale,
             [g_window firstResponder], g_input_view);
    return true;
}

// Reset all background colors from the startup fill to black.
// Called once when the first real content is about to be revealed.
static void reset_background_to_black() {
    g_mpv.SetBackgroundColor(kVideoBgColor.hex);
    g_window.backgroundColor = [NSColor blackColor];
    [[g_window contentView] layer].backgroundColor = [NSColor blackColor].CGColor;
}

static void macos_present(const CefAcceleratedPaintInfo& info) {
    if (g_transitioning) return;
    present_iosurface(g_main, info);
    // Player mode only: no overlay will ever paint, so the first main
    // frame is the reveal trigger. In normal mode this branch is a
    // no-op — macos_fade_overlay is responsible for unhiding the main
    // view when the overlay starts its fade-out.
    if (!g_overlay_client && [g_main.view isHidden]) {
        [g_main.view setHidden:NO];
        reset_background_to_black();
    }
    if (g_expected_w > 0) {
        IOSurfaceRef surface = (IOSurfaceRef)info.shared_texture_io_surface;
        if (surface && (int)IOSurfaceGetWidth(surface) == g_expected_w &&
            (int)IOSurfaceGetHeight(surface) == g_expected_h) {
            g_expected_w = 0; g_expected_h = 0;
            g_transitioning = false;
        }
    }
}

static void macos_present_software(const CefRenderHandler::RectList&, const void*, int, int) {}

static void macos_overlay_present(const CefAcceleratedPaintInfo& info) {
    present_iosurface(g_overlay, info);
}

static void macos_overlay_present_software(const CefRenderHandler::RectList&, const void*, int, int) {}

// The CALayer's frame tracks its hosting NSView via autoresizing. The
// IOSurface pool is recreated automatically inside present_iosurface
// when CEF starts delivering frames at a new pixel size, so there is
// nothing to do here. These entry points remain so the Platform
// dispatch table stays non-null.
static void macos_resize(int, int, int, int) {}
static void macos_overlay_resize(int, int, int, int) {}

static void macos_set_overlay_visible(bool visible) {
    g_overlay_visible = visible;
    [g_overlay.view setHidden:!visible];

    // Route keyboard focus to the newly-active browser. Without this, CEF
    // thinks the just-activated browser has no window focus, so text inputs
    // don't show a caret and focus rings don't render. Matches the "active
    // tab" semantics: only one browser at a time holds focus. Mirrors the
    // Wayland path in wl_set_overlay_visible.
    auto main = g_client ? g_client->browser() : nullptr;
    auto ovl  = g_overlay_client ? g_overlay_client->browser() : nullptr;
    if (visible) {
        if (main) main->GetHost()->SetFocus(false);
        if (ovl)  ovl->GetHost()->SetFocus(true);
    } else {
        if (ovl)  ovl->GetHost()->SetFocus(false);
        if (main) main->GetHost()->SetFocus(true);
    }
}

static void macos_fade_overlay(float delay_sec, float fade_sec,
                               std::function<void()> on_fade_start,
                               std::function<void()> on_complete) {
    if (!g_overlay.view || !g_overlay.view.layer) {
        if (on_fade_start) on_fade_start();
        if (on_complete) on_complete();
        return;
    }
    // Copy into block-friendly shared_ptrs so the callbacks survive into the block chain.
    auto start_cb = std::make_shared<std::function<void()>>(std::move(on_fade_start));
    auto done_cb = std::make_shared<std::function<void()>>(std::move(on_complete));
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay_sec * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        // Reveal the main browser view now — the delay has elapsed, the
        // fade is about to start, and g_main has had the full delay
        // duration to load content into its layer. Holds until this
        // moment so the user never sees the main browser before we've
        // committed to hiding the overlay. g_overlay is still fully
        // opaque at this point, so g_main is occluded until the fade
        // actually drops overlay opacity below 1.0 a few lines down.
        // Also reset mpv's clear color back to black — during startup
        // it was set to match the app's dark fill, but from here on
        // the video layer should use black as the default background.
        if ([g_main.view isHidden]) {
            [g_main.view setHidden:NO];
            reset_background_to_black();
        }
        if (*start_cb) (*start_cb)();
        if (!g_overlay.view || !g_overlay.view.layer) {
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
            macos_set_overlay_visible(false);
            [g_overlay.view.layer removeAllAnimations];
            g_overlay.view.layer.opacity = 1.0;
            if (*done_cb) (*done_cb)();
        }];
        [g_overlay.view.layer addAnimation:fade forKey:@"fadeOut"];
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
    // Drop cached input-surface wrappers so the next paint re-wraps at
    // the new size. The output pool is recreated automatically inside
    // ensure_pool() when the input dimensions change.
    g_main.input_texture = nil;
    g_main.cached_input = nullptr;
    g_overlay.input_texture = nil;
    g_overlay.cached_input = nullptr;
}

static void macos_end_transition() {}

static bool macos_in_transition() { return g_transitioning; }

static void macos_set_expected_size(int w, int h) {
    g_expected_w = w;
    g_expected_h = h;
}

static float macos_get_scale() {
    if (g_window) return static_cast<float>([g_window backingScaleFactor]);
    return 1.0f;
}

static bool macos_query_logical_content_size(int* w, int* h) {
    if (!g_window) return false;
    NSRect bounds = [[g_window contentView] bounds];
    *w = static_cast<int>(bounds.size.width);
    *h = static_cast<int>(bounds.size.height);
    return *w > 0 && *h > 0;
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
    // Clamp position so the window stays fully on-screen
    if (*x >= 0 && *x + *w > vw) *x = vw - *w;
    if (*y >= 0 && *y + *h > vh) *y = vh - *h;
    if (*x < 0) *x = -1;  // preserve "not set" sentinel
    if (*y < 0) *y = -1;
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

static void macos_set_titlebar_color(uint8_t, uint8_t, uint8_t) {
    // No-op on macOS (deferred)
}

static void macos_cleanup() {
    // Stop the display link first so no more BeginFrames race the teardown.
    stop_display_link();

    if (g_input_view)    { [g_input_view removeFromSuperview];    g_input_view = nil; }
    if (g_overlay.view)  { [g_overlay.view removeFromSuperview];  g_overlay.view = nil; }
    if (g_main.view)     { [g_main.view removeFromSuperview];     g_main.view = nil; }

    g_main.input_texture = nil;
    g_overlay.input_texture = nil;
    g_main.cached_input = nullptr;
    g_overlay.cached_input = nullptr;
    release_pool(g_main);
    release_pool(g_overlay);

    g_main.layer = nil;
    g_overlay.layer = nil;
    g_mtl_pipeline = nil; g_mtl_queue = nil; g_mtl_device = nil;
    g_window = nil;
}

static void macos_early_init() {
    [JellyfinApplication sharedApplication];

    // Subprocesses (GPU, renderer) only need CefAppProtocol — hide from dock
    if (getenv("JELLYFIN_CEF_SUBPROCESS")) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        return;
    }

    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Menu bar with Quit
    NSMenu* menubar = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:appMenuItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Quit"
                                                action:@selector(terminate:)
                                         keyEquivalent:@"q"]];
    [appMenuItem setSubmenu:appMenu];
    [NSApp setMainMenu:menubar];

    [NSApp finishLaunching];
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

Platform make_macos_platform() {
    return Platform{
        .early_init = macos_early_init,
        .init = macos_init,
        .cleanup = macos_cleanup,
        .present = macos_present,
        .present_software = macos_present_software,
        .resize = macos_resize,
        .overlay_present = macos_overlay_present,
        .overlay_present_software = macos_overlay_present_software,
        .overlay_resize = macos_overlay_resize,
        .set_overlay_visible = macos_set_overlay_visible,
        .fade_overlay = macos_fade_overlay,
        .set_fullscreen = macos_set_fullscreen,
        .toggle_fullscreen = macos_toggle_fullscreen,
        .begin_transition = macos_begin_transition,
        .end_transition = macos_end_transition,
        .in_transition = macos_in_transition,
        .set_expected_size = macos_set_expected_size,
        .get_scale = macos_get_scale,
        .query_logical_content_size = macos_query_logical_content_size,
        .query_window_position = macos_query_window_position,
        .clamp_window_geometry = macos_clamp_window_geometry,
        .pump = macos_pump,
        .run_main_loop = macos_run_main_loop,
        .wake_main_loop = macos_wake_main_loop,
        .set_cursor = input::macos::set_cursor,
        .set_idle_inhibit = macos_set_idle_inhibit,
        .set_titlebar_color = macos_set_titlebar_color,
        .clipboard_read_text_async = macos_clipboard_read_text_async,
    };
}
