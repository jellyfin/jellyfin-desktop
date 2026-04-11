// platform_macos.mm — macOS platform layer.
// Two CAMetalLayers composite CEF IOSurfaces (main + overlay) onto mpv's window.
// Input is owned by src/input/input_macos.mm.

#include "platform/platform.h"
#include "common.h"
#include "cef/cef_client.h"
#include "input/input_macos.h"

#include "include/cef_application_mac.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

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
// Metal compositor state (two layers: main + overlay)
// =====================================================================

static id<MTLDevice> g_mtl_device = nil;
static id<MTLCommandQueue> g_mtl_queue = nil;
static id<MTLRenderPipelineState> g_mtl_pipeline = nil;

// Main browser layer
static CAMetalLayer* g_main_layer = nil;
static NSView* g_main_view = nil;
static id<MTLTexture> g_main_texture = nil;
static IOSurfaceRef g_main_cached_surface = nullptr;

// Overlay browser layer
static CAMetalLayer* g_overlay_layer = nil;
static NSView* g_overlay_view = nil;
static id<MTLTexture> g_overlay_texture = nil;
static IOSurfaceRef g_overlay_cached_surface = nullptr;
static bool g_overlay_visible = false;

// Input NSView (owned by input::macos)
static NSView* g_input_view = nil;

// Window + transition state
static NSWindow* g_window = nullptr;
static int g_expected_w = 0, g_expected_h = 0;
static bool g_transitioning = false;

// Metal shaders (fullscreen triangle with premultiplied alpha)
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
// Metal rendering helper (shared by main + overlay layers)
// =====================================================================

static void metal_present_to_layer(CAMetalLayer* layer,
                                   id<MTLTexture> __strong& texture,
                                   IOSurfaceRef& cached_surface,
                                   const CefAcceleratedPaintInfo& info) {
    if (!g_mtl_device || !layer) return;

    IOSurfaceRef surface = (IOSurfaceRef)info.shared_texture_io_surface;
    if (!surface) return;

    int w = IOSurfaceGetWidth(surface);
    int h = IOSurfaceGetHeight(surface);

    if (surface != cached_surface) {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:w height:h mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        texture = [g_mtl_device newTextureWithDescriptor:desc iosurface:surface plane:0];
        if (!texture) return;
        cached_surface = surface;
        if (w != (int)layer.drawableSize.width || h != (int)layer.drawableSize.height)
            layer.drawableSize = CGSizeMake(w, h);
    }

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) return;
        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        passDesc.colorAttachments[0].texture = drawable.texture;
        passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
        passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
        passDesc.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLCommandBuffer> cmdBuf = [g_mtl_queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:passDesc];
        [enc setRenderPipelineState:g_mtl_pipeline];
        [enc setFragmentTexture:texture atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cmdBuf presentDrawable:drawable];
        [cmdBuf commit];
    }
}

// =====================================================================
// Helper: create a CAMetalLayer + hosting NSView
// =====================================================================

static void create_metal_layer(NSView* contentView, CGRect frame, CGFloat scale,
                               NSView* __strong& out_view, CAMetalLayer* __strong& out_layer,
                               NSView* positionAbove) {
    out_view = [[NSView alloc] initWithFrame:frame];
    [out_view setWantsLayer:YES];
    [out_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    out_layer = [CAMetalLayer layer];
    out_layer.device = g_mtl_device;
    out_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    out_layer.framebufferOnly = YES;
    out_layer.frame = frame;
    out_layer.drawableSize = CGSizeMake(frame.size.width * scale, frame.size.height * scale);
    out_layer.contentsScale = scale;
    out_layer.opaque = NO;
    out_layer.actions = @{
        @"bounds": [NSNull null], @"position": [NSNull null],
        @"contents": [NSNull null], @"anchorPoint": [NSNull null]
    };

    [out_view setLayer:out_layer];
    [contentView addSubview:out_view positioned:NSWindowAbove relativeTo:positionAbove];
}

// =====================================================================
// Platform interface implementation
// =====================================================================

static bool macos_init(mpv_handle* mpv) {
    for (int i = 0; i < 500 && !g_window; i++) {
        macos_pump();
        for (NSWindow* w in [NSApp windows]) {
            if ([w isVisible]) { g_window = w; break; }
        }
        if (!g_window) usleep(10000);
    }
    if (!g_window) {
        fprintf(stderr, "mpv did not create a window\n");
        return false;
    }

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

    // Metal setup
    g_mtl_device = MTLCreateSystemDefaultDevice();
    if (!g_mtl_device) { fprintf(stderr, "Metal device creation failed\n"); return false; }
    g_mtl_queue = [g_mtl_device newCommandQueue];

    NSError* error = nil;
    id<MTLLibrary> library = [g_mtl_device newLibraryWithSource:g_shader_source options:nil error:&error];
    if (!library) { fprintf(stderr, "Metal shader compile: %s\n", [[error localizedDescription] UTF8String]); return false; }

    MTLRenderPipelineDescriptor* pipeDesc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeDesc.vertexFunction = [library newFunctionWithName:@"vertexShader"];
    pipeDesc.fragmentFunction = [library newFunctionWithName:@"fragmentShader"];
    pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeDesc.colorAttachments[0].blendingEnabled = YES;
    pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    g_mtl_pipeline = [g_mtl_device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
    if (!g_mtl_pipeline) { fprintf(stderr, "Metal pipeline: %s\n", [[error localizedDescription] UTF8String]); return false; }

    // Create layers: main (bottom) → overlay (middle) → input (top)
    CGRect frame = [contentView bounds];
    CGFloat scale = [g_window backingScaleFactor];

    create_metal_layer(contentView, frame, scale, g_main_view, g_main_layer, nil);
    create_metal_layer(contentView, frame, scale, g_overlay_view, g_overlay_layer, g_main_view);
    [g_overlay_view setHidden:YES];

    g_input_view = input::macos::create_input_view();
    g_input_view.frame = contentView.bounds;
    g_input_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [contentView addSubview:g_input_view positioned:NSWindowAbove relativeTo:g_overlay_view];

    fprintf(stderr, "Metal compositor initialized (2 layers)\n");
    return true;
}

static void macos_present(const CefAcceleratedPaintInfo& info) {
    if (g_transitioning) return;
    metal_present_to_layer(g_main_layer, g_main_texture, g_main_cached_surface, info);
    if (g_expected_w > 0) {
        IOSurfaceRef surface = (IOSurfaceRef)info.shared_texture_io_surface;
        if (surface && (int)IOSurfaceGetWidth(surface) == g_expected_w &&
            (int)IOSurfaceGetHeight(surface) == g_expected_h) {
            g_expected_w = 0; g_expected_h = 0;
            g_transitioning = false;
        }
    }
}

static void macos_present_software(const void*, int, int) {}

static void macos_overlay_present(const CefAcceleratedPaintInfo& info) {
    metal_present_to_layer(g_overlay_layer, g_overlay_texture, g_overlay_cached_surface, info);
}

static void macos_overlay_present_software(const void*, int, int) {}

static void macos_resize(int lw, int lh, int, int) {
    if (!g_main_layer) return;
    CGFloat scale = [g_window backingScaleFactor];
    g_main_layer.drawableSize = CGSizeMake(lw * scale, lh * scale);
    g_main_layer.contentsScale = scale;
}

static void macos_overlay_resize(int lw, int lh, int, int) {
    if (!g_overlay_layer) return;
    CGFloat scale = [g_window backingScaleFactor];
    g_overlay_layer.drawableSize = CGSizeMake(lw * scale, lh * scale);
    g_overlay_layer.contentsScale = scale;
}

static void macos_set_overlay_visible(bool visible) {
    g_overlay_visible = visible;
    [g_overlay_view setHidden:!visible];
}

static void macos_fade_overlay(float delay_sec, float fade_sec,
                               std::function<void()> on_fade_start,
                               std::function<void()> on_complete) {
    if (!g_overlay_view || !g_overlay_view.layer) {
        if (on_fade_start) on_fade_start();
        if (on_complete) on_complete();
        return;
    }
    // Copy into block-friendly shared_ptrs so the callbacks survive into the block chain.
    auto start_cb = std::make_shared<std::function<void()>>(std::move(on_fade_start));
    auto done_cb = std::make_shared<std::function<void()>>(std::move(on_complete));
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay_sec * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        if (*start_cb) (*start_cb)();
        if (!g_overlay_view || !g_overlay_view.layer) {
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
            [g_overlay_view.layer removeAllAnimations];
            g_overlay_view.layer.opacity = 1.0;
            if (*done_cb) (*done_cb)();
        }];
        [g_overlay_view.layer addAnimation:fade forKey:@"fadeOut"];
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
    g_main_texture = nil;
    g_main_cached_surface = nullptr;
    g_overlay_texture = nil;
    g_overlay_cached_surface = nullptr;
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

static void macos_pump() {
    @autoreleasepool {
        // distantPast = return immediately if no event. `nil` means distantFuture
        // (block forever), which would freeze the main loop between poll() iterations.
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

static void macos_set_titlebar_color(uint8_t, uint8_t, uint8_t) {
    // No-op on macOS (deferred)
}

static void macos_cleanup() {
    if (g_input_view) { [g_input_view removeFromSuperview]; g_input_view = nil; }
    if (g_overlay_view) { [g_overlay_view removeFromSuperview]; g_overlay_view = nil; }
    if (g_main_view) { [g_main_view removeFromSuperview]; g_main_view = nil; }
    g_main_texture = nil; g_overlay_texture = nil;
    g_main_layer = nil; g_overlay_layer = nil;
    g_mtl_pipeline = nil; g_mtl_queue = nil; g_mtl_device = nil;
    g_main_cached_surface = nullptr; g_overlay_cached_surface = nullptr;
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
        .pump = macos_pump,
        .set_cursor = input::macos::set_cursor,
        .set_idle_inhibit = macos_set_idle_inhibit,
        .set_titlebar_color = macos_set_titlebar_color,
        .clipboard_read_text_async = macos_clipboard_read_text_async,
    };
}
