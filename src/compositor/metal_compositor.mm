#ifdef __APPLE__

#import "metal_compositor.h"
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#include <cstring>
#include "logging.h"

// Helper macros for void* casts
#define DEVICE ((__bridge id<MTLDevice>)device_)
#define CMD_QUEUE ((__bridge id<MTLCommandQueue>)command_queue_)
#define TEXTURE ((__bridge id<MTLTexture>)texture_)
#define PIPELINE ((__bridge id<MTLRenderPipelineState>)pipeline_state_)

// Simple vertex/fragment shaders for textured quad
static NSString* const shaderSource = @R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex VertexOut vertexShader(uint vertexID [[vertex_id]]) {
    // Full-screen triangle
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };
    float2 texCoords[3] = {
        float2(0.0, 1.0),
        float2(2.0, 1.0),
        float2(0.0, -1.0)
    };

    VertexOut out;
    out.position = float4(positions[vertexID], 0.0, 1.0);
    out.texCoord = texCoords[vertexID];
    return out;
}

fragment float4 fragmentShader(VertexOut in [[stage_in]],
                                texture2d<float> tex [[texture(0)]],
                                constant float& alpha [[buffer(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear);
    float4 color = tex.sample(s, in.texCoord);
    // Metal's BGRA8Unorm format already swizzles to RGBA when sampling
    color.a *= alpha;
    // Premultiply alpha for correct blending
    color.rgb *= color.a;
    return color;
}
)";

bool MetalCompositor::init(SDL_Window* window, uint32_t width, uint32_t height) {
    window_ = window;
    width_ = width;
    height_ = height;

    // Get NSWindow from SDL
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    NSWindow* ns_window = (__bridge NSWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!ns_window) {
        NSLog(@"MetalCompositor: Failed to get NSWindow");
        return false;
    }
    parent_window_ = ns_window;

    // Create Metal device
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        NSLog(@"MetalCompositor: Failed to create Metal device");
        return false;
    }
    device_ = (void*)CFBridgingRetain(device);

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) {
        NSLog(@"MetalCompositor: Failed to create command queue");
        return false;
    }
    command_queue_ = (void*)CFBridgingRetain(queue);

    // Create Metal layer for CEF overlay
    NSView* content_view = [ns_window contentView];
    NSRect frame = [content_view bounds];

    metal_view_ = [[NSView alloc] initWithFrame:frame];
    [metal_view_ setWantsLayer:YES];
    [metal_view_ setLayerContentsRedrawPolicy:NSViewLayerContentsRedrawDuringViewResize];
    [metal_view_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    metal_layer_ = [CAMetalLayer layer];
    metal_layer_.device = DEVICE;
    metal_layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    metal_layer_.framebufferOnly = YES;
    metal_layer_.frame = frame;
    metal_layer_.drawableSize = CGSizeMake(width, height);
    metal_layer_.contentsScale = [ns_window backingScaleFactor];
    metal_layer_.opaque = NO;  // Allow transparency for video to show through
    // Vsync throttle: with 2 drawables and displaySyncEnabled=YES (default),
    // nextDrawable blocks when both are in-flight — the Metal equivalent of
    // eglSwapBuffers blocking on Linux.  3 drawables (the default) rarely block
    // because the GPU finishes fast enough, letting the main loop spin.
    metal_layer_.maximumDrawableCount = 2;
    // presentsWithTransaction is toggled on during resize for fluid CA-synced presents.

    // Disable implicit animations to prevent jelly effect during resize
    metal_layer_.actions = @{
        @"bounds": [NSNull null],
        @"position": [NSNull null],
        @"contents": [NSNull null],
        @"anchorPoint": [NSNull null]
    };
    metal_layer_.contentsGravity = kCAGravityTopLeft;
    metal_layer_.anchorPoint = CGPointMake(0, 0);

    [metal_view_ setLayer:metal_layer_];

    // Ensure content view is layer-backed
    [content_view setWantsLayer:YES];

    // Add CEF overlay view on top of content view
    [content_view addSubview:metal_view_ positioned:NSWindowAbove relativeTo:nil];

    NSLog(@"MetalCompositor: added metal_view_ to content_view, subview count: %lu",
          (unsigned long)[[content_view subviews] count]);

    if (!createPipeline()) {
        return false;
    }

    if (!createTexture(width, height)) {
        return false;
    }

    // Allocate staging buffer
    staging_size_ = width * height * 4;
    staging_buffer_ = malloc(staging_size_);

    NSLog(@"MetalCompositor: initialized %dx%d", width, height);
    return true;
}

bool MetalCompositor::createPipeline() {
    NSError* error = nil;

    id<MTLLibrary> library = [DEVICE newLibraryWithSource:shaderSource options:nil error:&error];
    if (!library) {
        NSLog(@"MetalCompositor: Failed to compile shaders: %@", error);
        return false;
    }

    id<MTLFunction> vertexFunc = [library newFunctionWithName:@"vertexShader"];
    id<MTLFunction> fragmentFunc = [library newFunctionWithName:@"fragmentShader"];

    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = vertexFunc;
    desc.fragmentFunction = fragmentFunc;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

    // Premultiplied alpha blending
    desc.colorAttachments[0].blendingEnabled = YES;
    desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    id<MTLRenderPipelineState> pipelineState = [DEVICE newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pipelineState) {
        NSLog(@"MetalCompositor: Failed to create pipeline state: %@", error);
        return false;
    }
    pipeline_state_ = (void*)CFBridgingRetain(pipelineState);

    return true;
}

bool MetalCompositor::createTexture(uint32_t width, uint32_t height) {
    // Release old texture
    if (texture_) {
        CFBridgingRelease(texture_);
        texture_ = nullptr;
    }

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:width
                                                                                   height:height
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;

    id<MTLTexture> texture = [DEVICE newTextureWithDescriptor:desc];
    if (!texture) {
        NSLog(@"MetalCompositor: Failed to create texture");
        return false;
    }
    texture_ = (void*)CFBridgingRetain(texture);
    // Keep has_content_ - CEF will repaint at new size via CefDoMessageLoopWork

    return true;
}

void MetalCompositor::cleanup() {
    // Release any pending queued IOSurface
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queued_iosurface_.surface) {
            CFRelease((IOSurfaceRef)queued_iosurface_.surface);
            queued_iosurface_.surface = nullptr;
        }
        iosurface_pending_.store(false, std::memory_order_relaxed);
    }

    if (metal_view_) {
        [metal_view_ removeFromSuperview];
        metal_view_ = nil;
    }
    metal_layer_ = nil;

    if (texture_) {
        CFBridgingRelease(texture_);
        texture_ = nullptr;
    }
    if (pipeline_state_) {
        CFBridgingRelease(pipeline_state_);
        pipeline_state_ = nullptr;
    }
    if (command_queue_) {
        CFBridgingRelease(command_queue_);
        command_queue_ = nullptr;
    }
    if (device_) {
        CFBridgingRelease(device_);
        device_ = nullptr;
    }

    if (staging_buffer_) {
        free(staging_buffer_);
        staging_buffer_ = nullptr;
    }
}

void MetalCompositor::updateOverlayPartial(const void* data, int src_width, int src_height) {
    if (!data || src_width <= 0 || src_height <= 0) {
        return;
    }

    // Resize if dimensions don't match
    if (src_width != (int)width_ || src_height != (int)height_) {
        resize(src_width, src_height);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (staging_buffer_) {
        memcpy(staging_buffer_, data, src_width * src_height * 4);
        staging_dirty_ = true;
    }
}

void MetalCompositor::queueIOSurface(void* ioSurface, int format, int width, int height) {
    if (!ioSurface || width <= 0 || height <= 0) {
        return;
    }

    static bool first = true;
    if (first) {
        LOG_INFO(LOG_COMPOSITOR, "MetalCompositor: first IOSurface queued %dx%d format=%d", width, height, format);
        first = false;
    }

    IOSurfaceRef surface = (IOSurfaceRef)ioSurface;

    std::lock_guard<std::mutex> lock(mutex_);

    // Release any previously queued surface that wasn't imported
    if (iosurface_pending_.load(std::memory_order_relaxed) && queued_iosurface_.surface) {
        CFRelease((IOSurfaceRef)queued_iosurface_.surface);
    }

    // Retain the surface - CEF may release it after callback returns
    CFRetain(surface);

    queued_iosurface_.surface = surface;
    queued_iosurface_.format = format;
    queued_iosurface_.width = width;
    queued_iosurface_.height = height;
    iosurface_pending_.store(true, std::memory_order_release);
}

bool MetalCompositor::importQueuedIOSurface() {
    // Fast-path: check atomic without lock
    if (!iosurface_pending_.load(std::memory_order_acquire)) {
        return false;
    }

    // Get queued IOSurface under lock
    IOSurfaceRef surface;
    int format, w, h;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!iosurface_pending_.load(std::memory_order_relaxed)) {
            return false;  // Another thread got it
        }
        surface = (IOSurfaceRef)queued_iosurface_.surface;
        format = queued_iosurface_.format;
        w = queued_iosurface_.width;
        h = queued_iosurface_.height;
        iosurface_pending_.store(false, std::memory_order_relaxed);
        queued_iosurface_.surface = nullptr;
    }

    if (!surface) {
        return false;
    }

    // Check if this is the same surface as before - skip texture recreation
    IOSurfaceID surfaceID = IOSurfaceGetID(surface);
    if (surfaceID == cached_surface_id_ && texture_) {
        // Same surface — check if content actually changed
        uint32_t seed = IOSurfaceGetSeed(surface);
        CFRelease(surface);
        if (seed == cached_surface_seed_) {
            return false;  // No visual change — skip compositing
        }
        cached_surface_seed_ = seed;
        has_content_ = true;
        return true;
    }

    // Update layer drawable size if dimensions changed
    if (w != (int)width_ || h != (int)height_) {
        width_ = w;
        height_ = h;
        if (metal_layer_) {
            metal_layer_.drawableSize = CGSizeMake(w, h);
        }
    }

    // Release old texture
    if (texture_) {
        CFBridgingRelease(texture_);
        texture_ = nullptr;
    }

    // Create texture descriptor matching IOSurface properties
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:w
                                                                                   height:h
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;  // IOSurface is shared memory

    // Record seed before releasing our reference (texture retains the surface internally)
    uint32_t seed = IOSurfaceGetSeed(surface);

    // Create texture directly from IOSurface - zero-copy!
    id<MTLTexture> texture = [DEVICE newTextureWithDescriptor:desc iosurface:surface plane:0];

    // Done with the retained surface
    CFRelease(surface);

    if (!texture) {
        LOG_ERROR(LOG_COMPOSITOR, "MetalCompositor: failed to create texture from IOSurface");
        return false;
    }

    texture_ = (void*)CFBridgingRetain(texture);
    cached_surface_id_ = surfaceID;
    cached_surface_seed_ = seed;
    has_content_ = true;
    staging_dirty_ = false;  // No staging buffer upload needed

    static bool first_import = true;
    if (first_import) {
        LOG_INFO(LOG_COMPOSITOR, "MetalCompositor: first IOSurface imported %dx%d", w, h);
        first_import = false;
    }

    return true;
}

void MetalCompositor::composite(uint32_t width, uint32_t height, float alpha) {
    (void)width;
    (void)height;

    std::lock_guard<std::mutex> lock(mutex_);

    // Upload staging buffer to texture if dirty
    if (staging_dirty_ && staging_buffer_ && texture_) {
        MTLRegion region = MTLRegionMake2D(0, 0, width_, height_);
        [TEXTURE replaceRegion:region mipmapLevel:0 withBytes:staging_buffer_ bytesPerRow:width_ * 4];
        staging_dirty_ = false;
        has_content_ = true;
        static bool first_content = true;
        if (first_content) {
            NSLog(@"MetalCompositor: first content received");
            first_content = false;
        }
    }

    if (!has_content_) {
        return;
    }

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [metal_layer_ nextDrawable];
        if (!drawable) {
            NSLog(@"MetalCompositor: nextDrawable returned nil");
            return;
        }

        static bool first_draw = true;
        if (first_draw) {
            NSLog(@"MetalCompositor: first draw, layer frame: %@, hidden: %d",
                  NSStringFromRect(metal_layer_.frame), metal_view_.hidden);
            first_draw = false;
        }

        MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
        passDesc.colorAttachments[0].texture = drawable.texture;
        passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
        passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
        passDesc.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);  // Transparent

        id<MTLCommandBuffer> commandBuffer = [CMD_QUEUE commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:passDesc];

        [encoder setRenderPipelineState:PIPELINE];
        [encoder setFragmentTexture:TEXTURE atIndex:0];
        [encoder setFragmentBytes:&alpha length:sizeof(float) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];

        if (metal_layer_.presentsWithTransaction) {
            // Resize mode: sync present with CA transaction for fluid resize
            [commandBuffer commit];
            [commandBuffer waitUntilScheduled];
            [drawable present];
        } else {
            // Normal mode: async present at next vblank (displaySyncEnabled paces frames)
            [commandBuffer presentDrawable:drawable];
            [commandBuffer commit];
        }
    }
}

void MetalCompositor::setPresentsWithTransaction(bool enabled) {
    if (metal_layer_) {
        metal_layer_.presentsWithTransaction = enabled;
    }
}

void MetalCompositor::setVisible(bool visible) {
    if (metal_view_) {
        [metal_view_ setHidden:!visible];
    }
}

void MetalCompositor::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    width_ = width;
    height_ = height;

    // Update layer drawable size (layer frame auto-resizes with view)
    if (metal_layer_) {
        metal_layer_.drawableSize = CGSizeMake(width, height);
        if (parent_window_) {
            metal_layer_.contentsScale = [parent_window_ backingScaleFactor];
        }
    }

    // Recreate texture and staging buffer
    createTexture(width, height);

    if (staging_buffer_) {
        free(staging_buffer_);
    }
    staging_size_ = width * height * 4;
    staging_buffer_ = malloc(staging_size_);
}

#endif // __APPLE__
