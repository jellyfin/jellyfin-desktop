//! CAMetalLayer-based per-surface compositor — native Rust port of the
//! C++ originals in `src/platform/macos.mm`.
//!
//! All AppKit operations must run on the main thread; if Browsers calls
//! alloc/free/restack/resize/set_visible off-main we `dispatch_sync` (or
//! `dispatch_async` for fire-and-forget) onto the main queue. CEF's
//! accelerated-paint thread calls `surface_present` directly — CAMetalLayer
//! tolerates `nextDrawable` + Metal command-encoding off the main thread,
//! but layer-tree mutations (adding subviews, setting frame) still need
//! main-thread dispatch.
//!
//! Per-surface state is owned by `Box<Surface>`. The opaque pointer
//! returned from `macos_alloc_surface` is `Box::into_raw`; `macos_free_surface`
//! reconstitutes via `Box::from_raw` after detaching the AppKit subview.

use std::ffi::{c_int, c_void};
use std::ptr;
use std::sync::Mutex;

use objc2::encode::{Encode, Encoding};
use objc2::runtime::AnyObject;

use crate::G_IN_TRANSITION;

unsafe extern "C" {
    /// Accessors implemented in src/platform/macos.mm.
    fn jfn_macos_get_window() -> *mut AnyObject;
    fn jfn_macos_get_input_view() -> *mut AnyObject;

    static _dispatch_main_q: c_void;
    fn dispatch_async_f(
        queue: *mut c_void,
        ctx: *mut c_void,
        work: unsafe extern "C" fn(*mut c_void),
    );
}

#[inline]
fn dispatch_get_main_queue() -> *mut c_void {
    unsafe { std::ptr::addr_of!(_dispatch_main_q) as *mut c_void }
}

fn is_main_thread() -> bool {
    unsafe {
        let cls = objc2::class!(NSThread);
        let b: bool = objc2::msg_send![cls, isMainThread];
        b
    }
}

// =====================================================================
// CoreFoundation / CoreGraphics / IOSurface / Metal externs.
//
// We avoid pulling in objc2-quartz-core / objc2-metal / objc2-io-surface
// for parity with the rest of this crate (raw msg_send! plus narrow
// extern decls). Classes are looked up via `objc2::class!` and messages
// dispatched via `objc2::msg_send!`.
// =====================================================================

type CFTypeRef = *const c_void;
type CFStringRef = *const c_void;
type CFTypeID = usize;
type CGColorSpaceRef = *mut c_void;
type IOSurfaceRef = *mut c_void;

unsafe extern "C" {
    fn CFGetTypeID(cf: CFTypeRef) -> CFTypeID;
    fn CFStringGetTypeID() -> CFTypeID;
    fn CFRelease(cf: CFTypeRef);

    fn CGColorSpaceCreateWithName(name: CFStringRef) -> CGColorSpaceRef;
    fn CGColorSpaceRelease(cs: CGColorSpaceRef);

    static kCGColorSpaceSRGB: CFStringRef;

    fn IOSurfaceGetWidth(surface: IOSurfaceRef) -> usize;
    fn IOSurfaceGetHeight(surface: IOSurfaceRef) -> usize;
    fn IOSurfaceCopyValue(surface: IOSurfaceRef, key: CFStringRef) -> CFTypeRef;

    static kIOSurfaceColorSpace: CFStringRef;

    // dispatch_sync_f — bounce onto the main queue and block until the
    // work item returns. Used by macos_alloc_surface / macos_free_surface
    // / macos_restack which need their AppKit mutations done before
    // returning to the caller.
    fn dispatch_sync_f(
        queue: *mut c_void,
        ctx: *mut c_void,
        work: unsafe extern "C" fn(*mut c_void),
    );
}

// =====================================================================
// Geometry types (NSRect / CGSize / CGPoint). We use objc2-foundation's
// NSRect to match the rest of the crate; CGSize / CGPoint are repr(C).
// =====================================================================

#[repr(C)]
#[derive(Clone, Copy)]
struct CGSize {
    width: f64,
    height: f64,
}

unsafe impl Encode for CGSize {
    const ENCODING: Encoding =
        Encoding::Struct("CGSize", &[f64::ENCODING, f64::ENCODING]);
}

// =====================================================================
// MTLPixelFormatBGRA8Unorm = 80. MTLLoadActionClear=2, MTLStoreActionStore=1.
// MTLPrimitiveTypeTriangle = 3. MTLTextureUsageShaderRead = 1.
// MTLStorageModeShared = 0. (Enum values stable since Metal 1.0.)
// =====================================================================

const MTL_PIXEL_FORMAT_BGRA8_UNORM: u64 = 80;
const MTL_LOAD_ACTION_CLEAR: u64 = 2;
const MTL_STORE_ACTION_STORE: u64 = 1;
const MTL_PRIMITIVE_TYPE_TRIANGLE: u64 = 3;
const MTL_TEXTURE_USAGE_SHADER_READ: u64 = 1;
const MTL_STORAGE_MODE_SHARED: u64 = 0;

#[repr(C)]
#[derive(Clone, Copy)]
struct MTLClearColor {
    red: f64,
    green: f64,
    blue: f64,
    alpha: f64,
}

unsafe impl Encode for MTLClearColor {
    const ENCODING: Encoding = Encoding::Struct(
        "",
        &[f64::ENCODING, f64::ENCODING, f64::ENCODING, f64::ENCODING],
    );
}

// =====================================================================
// Per-surface state. One per CefLayer (allocated by macos_alloc_surface,
// destroyed by macos_free_surface). Mirrors the C++ PlatformSurface in
// src/platform/macos.mm prior to this port.
// =====================================================================

struct Surface {
    /// NSView hosting `layer`. Owned (+1 retain) when non-null.
    view: *mut AnyObject,
    /// CAMetalLayer the present path renders into. Owned by `view`'s
    /// layer property; non-retained here.
    layer: *mut AnyObject,
    /// Cached input IOSurface identity — when the next CEF frame
    /// arrives on the same IOSurface we skip the texture re-wrap.
    cached_input: IOSurfaceRef,
    /// MTLTexture wrapping `cached_input` for sampling. Owned (+1
    /// retain).
    input_texture: *mut AnyObject,
}

unsafe impl Send for Surface {}

impl Surface {
    fn new() -> Self {
        Self {
            view: ptr::null_mut(),
            layer: ptr::null_mut(),
            cached_input: ptr::null_mut(),
            input_texture: ptr::null_mut(),
        }
    }

    /// Drop the cached input-texture wrapper so the next paint re-wraps
    /// at the (possibly new) IOSurface size.
    fn drop_input_texture(&mut self) {
        if !self.input_texture.is_null() {
            unsafe { let _: () = objc2::msg_send![self.input_texture, release]; }
            self.input_texture = ptr::null_mut();
        }
        self.cached_input = ptr::null_mut();
    }
}

// =====================================================================
// Surface registry — current stack order bottom-to-top, as last applied
// via macos_restack. stack[0] is the cef-main surface for transition
// gating in macos_surface_present.
//
// Stored as raw *mut Surface (not Box) because the same pointer is
// handed to / from C/Rust callers across the vtable. We Box::from_raw
// only when macos_free_surface is called.
// =====================================================================

struct StackEntry(*mut Surface);
unsafe impl Send for StackEntry {}

static G_SURFACE_STACK: Mutex<Vec<StackEntry>> = Mutex::new(Vec::new());

// Expected post-transition size. set_expected_size writes it; the
// present path clears it (and the transition flag) when an incoming
// IOSurface matches the expected dimensions.
static G_EXPECTED_SIZE: Mutex<(c_int, c_int)> = Mutex::new((0, 0));

// =====================================================================
// Metal device / queue / pipeline. Lazy-init on first alloc_surface so
// macos_init no longer has to bring up Metal.
// =====================================================================

struct MetalState {
    device: *mut AnyObject,       // id<MTLDevice>, retained
    queue: *mut AnyObject,        // id<MTLCommandQueue>, retained
    pipeline: *mut AnyObject,     // id<MTLRenderPipelineState>, retained
}
unsafe impl Send for MetalState {}

static G_METAL: Mutex<Option<MetalState>> = Mutex::new(None);

const SHADER_SRC: &str = r#"
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
"#;

unsafe extern "C" {
    /// MTLCreateSystemDefaultDevice is a free function in /System/Library/Frameworks/Metal.framework.
    fn MTLCreateSystemDefaultDevice() -> *mut AnyObject;
}

/// Build an NSString from a Rust &str (UTF-8). The returned object is
/// retained (+1) by NSString init; the caller `release`s when done.
unsafe fn nsstring_from_str(s: &str) -> *mut AnyObject {
    unsafe {
        let bytes = s.as_bytes();
        let alloc: *mut AnyObject =
            objc2::msg_send![objc2::class!(NSString), alloc];
        let init: *mut AnyObject = objc2::msg_send![
            alloc,
            initWithBytes: bytes.as_ptr() as *const c_void,
            length: bytes.len(),
            encoding: 4u64 // NSUTF8StringEncoding
        ];
        init
    }
}

/// Lazy-init Metal device, command queue and the straight→premultiplied
/// alpha render pipeline. Returns None on driver failure (logged once).
fn ensure_metal() -> bool {
    let mut guard = G_METAL.lock().unwrap();
    if guard.is_some() {
        return true;
    }
    unsafe {
        let device = MTLCreateSystemDefaultDevice();
        if device.is_null() {
            log::error!("[METAL] MTLCreateSystemDefaultDevice failed");
            return false;
        }
        let queue: *mut AnyObject = objc2::msg_send![device, newCommandQueue];
        if queue.is_null() {
            let _: () = objc2::msg_send![device, release];
            log::error!("[METAL] newCommandQueue failed");
            return false;
        }

        // Compile the shader source into an MTLLibrary.
        let src_ns = nsstring_from_str(SHADER_SRC);
        let mut err: *mut AnyObject = ptr::null_mut();
        let library: *mut AnyObject = objc2::msg_send![
            device,
            newLibraryWithSource: src_ns,
            options: ptr::null_mut::<AnyObject>(),
            error: &mut err as *mut *mut AnyObject
        ];
        let _: () = objc2::msg_send![src_ns, release];
        if library.is_null() {
            log::error!("[METAL] newLibraryWithSource failed");
            let _: () = objc2::msg_send![queue, release];
            let _: () = objc2::msg_send![device, release];
            return false;
        }

        let vfn_name = nsstring_from_str("vertexShader");
        let ffn_name = nsstring_from_str("fragmentShader");
        let vfn: *mut AnyObject = objc2::msg_send![library, newFunctionWithName: vfn_name];
        let ffn: *mut AnyObject = objc2::msg_send![library, newFunctionWithName: ffn_name];
        let _: () = objc2::msg_send![vfn_name, release];
        let _: () = objc2::msg_send![ffn_name, release];

        // MTLRenderPipelineDescriptor: vertex/fragment fn + colour
        // attachment 0 = BGRA8Unorm, blendingEnabled = NO.
        let pipe_desc_cls = objc2::class!(MTLRenderPipelineDescriptor);
        let pipe_desc: *mut AnyObject = objc2::msg_send![pipe_desc_cls, alloc];
        let pipe_desc: *mut AnyObject = objc2::msg_send![pipe_desc, init];
        let _: () = objc2::msg_send![pipe_desc, setVertexFunction: vfn];
        let _: () = objc2::msg_send![pipe_desc, setFragmentFunction: ffn];

        let attachments: *mut AnyObject = objc2::msg_send![pipe_desc, colorAttachments];
        let attach0: *mut AnyObject = objc2::msg_send![attachments, objectAtIndexedSubscript: 0usize];
        let _: () =
            objc2::msg_send![attach0, setPixelFormat: MTL_PIXEL_FORMAT_BGRA8_UNORM];
        let _: () = objc2::msg_send![attach0, setBlendingEnabled: false];

        let mut err2: *mut AnyObject = ptr::null_mut();
        let pipeline: *mut AnyObject = objc2::msg_send![
            device,
            newRenderPipelineStateWithDescriptor: pipe_desc,
            error: &mut err2 as *mut *mut AnyObject
        ];
        let _: () = objc2::msg_send![vfn, release];
        let _: () = objc2::msg_send![ffn, release];
        let _: () = objc2::msg_send![pipe_desc, release];
        let _: () = objc2::msg_send![library, release];
        if pipeline.is_null() {
            log::error!("[METAL] newRenderPipelineStateWithDescriptor failed");
            let _: () = objc2::msg_send![queue, release];
            let _: () = objc2::msg_send![device, release];
            return false;
        }

        *guard = Some(MetalState {
            device,
            queue,
            pipeline,
        });
        true
    }
}

/// Run a closure on the AppKit main thread. Used for layer-tree mutations
/// (subview attach, frame writes, etc.). Sync — caller blocks until the
/// closure returns. Safe to call from the main thread (runs inline).
///
/// The closure runs strictly on the main thread; raw pointers it
/// captures don't actually cross threads (sync blocks until the work
/// item returns). We therefore drop the `Send` bound and shuttle the
/// closure pointer through `usize` to satisfy dispatch_sync_f's C ABI.
fn run_on_main_sync<F>(f: F)
where
    F: FnOnce(),
{
    if is_main_thread() {
        f();
        return;
    }
    let boxed: Box<dyn FnOnce()> = Box::new(f);
    let dbl_box: Box<Box<dyn FnOnce()>> = Box::new(boxed);
    let ptr_ctx = Box::into_raw(dbl_box) as *mut c_void;
    unsafe extern "C" fn trampoline(ctx: *mut c_void) {
        unsafe {
            let dbl_box: Box<Box<dyn FnOnce()>> = Box::from_raw(ctx as *mut _);
            let inner = *dbl_box;
            inner();
        }
    }
    unsafe { dispatch_sync_f(dispatch_get_main_queue(), ptr_ctx, trampoline) };
}

/// Async version — fire-and-forget; the closure runs later on the main
/// queue. Used by macos_surface_set_visible / macos_surface_resize where
/// the caller does not need ordering. Closure must be `'static`.
fn run_on_main_async<F>(f: F)
where
    F: FnOnce() + 'static,
{
    if is_main_thread() {
        f();
        return;
    }
    let boxed: Box<dyn FnOnce()> = Box::new(f);
    let dbl_box: Box<Box<dyn FnOnce()>> = Box::new(boxed);
    let ptr_ctx = Box::into_raw(dbl_box) as *mut c_void;
    unsafe extern "C" fn trampoline(ctx: *mut c_void) {
        unsafe {
            let dbl_box: Box<Box<dyn FnOnce()>> = Box::from_raw(ctx as *mut _);
            let inner = *dbl_box;
            inner();
        }
    }
    unsafe { dispatch_async_f(dispatch_get_main_queue(), ptr_ctx, trampoline) };
}

// =====================================================================
// CAMetalLayer + NSView creation. Called from main-thread context only
// (run_on_main_sync inside macos_alloc_surface).
// =====================================================================

unsafe fn create_content_layer(
    content_view: *mut AnyObject,
    frame: objc2_foundation::NSRect,
    scale: f64,
) -> (*mut AnyObject, *mut AnyObject) {
    unsafe {
        let device = G_METAL.lock().unwrap().as_ref().unwrap().device;

        // NSView alloc/initWithFrame:
        let view_cls = objc2::class!(NSView);
        let view: *mut AnyObject = objc2::msg_send![view_cls, alloc];
        let view: *mut AnyObject = objc2::msg_send![view, initWithFrame: frame];
        let _: () = objc2::msg_send![view, setWantsLayer: true];
        // NSViewWidthSizable | NSViewHeightSizable = 2 | 16 (per AppKit).
        let _: () = objc2::msg_send![view, setAutoresizingMask: 2u64 | 16u64];

        // CAMetalLayer alloc + configure.
        let layer_cls = objc2::class!(CAMetalLayer);
        let layer: *mut AnyObject = objc2::msg_send![layer_cls, layer];
        let _: () = objc2::msg_send![layer, setDevice: device];
        let _: () = objc2::msg_send![layer, setPixelFormat: MTL_PIXEL_FORMAT_BGRA8_UNORM];

        let srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        let _: () = objc2::msg_send![layer, setColorspace: srgb];
        if !srgb.is_null() {
            CGColorSpaceRelease(srgb);
        }

        let _: () = objc2::msg_send![layer, setFramebufferOnly: true];
        let _: () = objc2::msg_send![layer, setFrame: frame];
        let _: () = objc2::msg_send![layer, setContentsScale: scale];
        let _: () = objc2::msg_send![layer, setOpaque: false];

        // Disable implicit animations on property changes — present
        // writes contents every frame and CA shouldn't cross-fade them.
        // Build an NSDictionary { "bounds": NSNull, ... }.
        let null_cls = objc2::class!(NSNull);
        let null_obj: *mut AnyObject = objc2::msg_send![null_cls, null];
        let dict_cls = objc2::class!(NSMutableDictionary);
        let dict: *mut AnyObject = objc2::msg_send![dict_cls, dictionaryWithCapacity: 5usize];
        for key in &["bounds", "position", "contents", "anchorPoint", "contentsRect"] {
            let k = nsstring_from_str(key);
            let _: () = objc2::msg_send![dict, setObject: null_obj, forKey: k];
            let _: () = objc2::msg_send![k, release];
        }
        let _: () = objc2::msg_send![layer, setActions: dict];

        let _: () = objc2::msg_send![view, setLayer: layer];

        // addSubview:positioned:relativeTo: — order applied by
        // macos_restack later; positionAbove=nil here.
        // NSWindowAbove == 1.
        let _: () = objc2::msg_send![
            content_view,
            addSubview: view,
            positioned: 1u64,
            relativeTo: ptr::null_mut::<AnyObject>(),
        ];

        (view, layer)
    }
}

// =====================================================================
// Input-IOSurface caching. Wrap the CEF IOSurface as an MTLTexture for
// sampling; recreate when identity changes. Updates the layer's
// colorspace from the IOSurface's kIOSurfaceColorSpace tag.
// =====================================================================

unsafe fn wrap_input_surface(s: &mut Surface, surface: IOSurfaceRef, w: u64, h: u64) -> bool {
    if surface == s.cached_input && !s.input_texture.is_null() {
        return true;
    }
    unsafe {
        let device = match G_METAL.lock().unwrap().as_ref() {
            Some(m) => m.device,
            None => return false,
        };
        let desc_cls = objc2::class!(MTLTextureDescriptor);
        let desc: *mut AnyObject = objc2::msg_send![
            desc_cls,
            texture2DDescriptorWithPixelFormat: MTL_PIXEL_FORMAT_BGRA8_UNORM,
            width: w as usize,
            height: h as usize,
            mipmapped: false
        ];
        let _: () = objc2::msg_send![desc, setUsage: MTL_TEXTURE_USAGE_SHADER_READ];
        let _: () = objc2::msg_send![desc, setStorageMode: MTL_STORAGE_MODE_SHARED];

        let tex: *mut AnyObject = objc2::msg_send![
            device,
            newTextureWithDescriptor: desc,
            iosurface: surface,
            plane: 0usize
        ];
        if tex.is_null() {
            log::error!("[METAL] wrap input IOSurface failed");
            return false;
        }
        // Replace cached texture (release previous).
        if !s.input_texture.is_null() {
            let _: () = objc2::msg_send![s.input_texture, release];
        }
        s.input_texture = tex;
        s.cached_input = surface;

        let cs = IOSurfaceCopyValue(surface, kIOSurfaceColorSpace);
        if !cs.is_null() && CFGetTypeID(cs) == CFStringGetTypeID() {
            let cg = CGColorSpaceCreateWithName(cs as CFStringRef);
            if !cg.is_null() {
                let _: () = objc2::msg_send![s.layer, setColorspace: cg];
                CGColorSpaceRelease(cg);
            }
        }
        if !cs.is_null() {
            CFRelease(cs);
        }
        true
    }
}

// =====================================================================
// Present: render CEF's straight-alpha IOSurface into the CAMetalLayer's
// next drawable with premultiplied alpha. Off-main-thread safe.
// =====================================================================

unsafe fn present_iosurface(s: &mut Surface, info: &cef_dll_sys::_cef_accelerated_paint_info_t) {
    if s.layer.is_null() {
        log::warn!("[METAL] present skipped: layer null");
        return;
    }
    let metal_q = match G_METAL.lock().unwrap().as_ref() {
        Some(m) => m.queue,
        None => {
            log::warn!("[METAL] present skipped: metal not initialized");
            return;
        }
    };
    let metal_pipe = G_METAL.lock().unwrap().as_ref().unwrap().pipeline;

    let surface = info.shared_texture_io_surface as IOSurfaceRef;
    if surface.is_null() {
        log::warn!("[METAL] present skipped: null IOSurface");
        return;
    }
    let w = unsafe { IOSurfaceGetWidth(surface) } as u64;
    let h = unsafe { IOSurfaceGetHeight(surface) } as u64;

    if !unsafe { wrap_input_surface(s, surface, w, h) } {
        return;
    }

    unsafe {
        // Update drawableSize if needed.
        let cur: CGSize = objc2::msg_send![s.layer, drawableSize];
        if cur.width as u64 != w || cur.height as u64 != h {
            let _: () = objc2::msg_send![
                s.layer,
                setDrawableSize: CGSize { width: w as f64, height: h as f64 }
            ];
        }

        // @autoreleasepool — bracket the per-frame AppKit allocations.
        let pool: *mut AnyObject =
            objc2::msg_send![objc2::class!(NSAutoreleasePool), new];

        let drawable: *mut AnyObject = objc2::msg_send![s.layer, nextDrawable];
        if drawable.is_null() {
            log::warn!("[METAL] nextDrawable returned nil");
            let _: () = objc2::msg_send![pool, drain];
            return;
        }
        let drawable_tex: *mut AnyObject = objc2::msg_send![drawable, texture];

        let pass_cls = objc2::class!(MTLRenderPassDescriptor);
        let pass: *mut AnyObject = objc2::msg_send![pass_cls, renderPassDescriptor];
        let attachments: *mut AnyObject = objc2::msg_send![pass, colorAttachments];
        let attach0: *mut AnyObject =
            objc2::msg_send![attachments, objectAtIndexedSubscript: 0usize];
        let _: () = objc2::msg_send![attach0, setTexture: drawable_tex];
        let _: () = objc2::msg_send![attach0, setLoadAction: MTL_LOAD_ACTION_CLEAR];
        let _: () = objc2::msg_send![attach0, setStoreAction: MTL_STORE_ACTION_STORE];
        let _: () = objc2::msg_send![
            attach0,
            setClearColor: MTLClearColor { red: 0.0, green: 0.0, blue: 0.0, alpha: 0.0 }
        ];

        let cmd_buf: *mut AnyObject = objc2::msg_send![metal_q, commandBuffer];
        let enc: *mut AnyObject =
            objc2::msg_send![cmd_buf, renderCommandEncoderWithDescriptor: pass];
        let _: () = objc2::msg_send![enc, setRenderPipelineState: metal_pipe];
        let _: () = objc2::msg_send![enc, setFragmentTexture: s.input_texture, atIndex: 0usize];
        let _: () = objc2::msg_send![
            enc,
            drawPrimitives: MTL_PRIMITIVE_TYPE_TRIANGLE,
            vertexStart: 0usize,
            vertexCount: 3usize
        ];
        let _: () = objc2::msg_send![enc, endEncoding];
        let _: () = objc2::msg_send![cmd_buf, presentDrawable: drawable];
        let _: () = objc2::msg_send![cmd_buf, commit];

        let _: () = objc2::msg_send![pool, drain];
    }
}

// =====================================================================
// Transition helpers
// =====================================================================

pub(crate) fn drop_input_textures() {
    let stack = G_SURFACE_STACK.lock().unwrap();
    for entry in stack.iter() {
        if entry.0.is_null() {
            continue;
        }
        unsafe { (*entry.0).drop_input_texture() };
    }
}

// =====================================================================
// Vtable-exposed compositor functions
// =====================================================================

#[unsafe(no_mangle)]
pub extern "C" fn macos_set_expected_size(w: c_int, h: c_int) {
    *G_EXPECTED_SIZE.lock().unwrap() = (w, h);
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_alloc_surface() -> *mut c_void {
    // Allocate the Surface up front; the AppKit setup happens on the
    // main thread but writes into this stable heap address.
    let surf_ptr = Box::into_raw(Box::new(Surface::new()));
    if !ensure_metal() {
        // Allocation must still return a valid opaque handle so Browsers
        // can later free it; the surface will simply have no layer.
        return surf_ptr as *mut c_void;
    }

    let s_addr = surf_ptr as usize;
    run_on_main_sync(move || unsafe {
        let win = jfn_macos_get_window();
        if win.is_null() {
            return;
        }
        let content_view: *mut AnyObject = objc2::msg_send![win, contentView];
        if content_view.is_null() {
            return;
        }
        let frame: objc2_foundation::NSRect = objc2::msg_send![content_view, bounds];
        let scale: f64 = objc2::msg_send![win, backingScaleFactor];
        let (view, layer) = create_content_layer(content_view, frame, scale);
        let surf = &mut *(s_addr as *mut Surface);
        surf.view = view;
        surf.layer = layer;
    });
    surf_ptr as *mut c_void
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_free_surface(s: *mut c_void) {
    if s.is_null() {
        return;
    }
    let s_ptr = s as *mut Surface;

    // Remove from the stack first — keeps is-cef-main coherent while
    // we're tearing down. Browsers normally restacks to a smaller order
    // right after this, but the defensive remove matches the C++ path.
    {
        let mut stack = G_SURFACE_STACK.lock().unwrap();
        stack.retain(|e| e.0 != s_ptr);
    }

    let s_addr = s_ptr as usize;
    run_on_main_sync(move || unsafe {
        let surf = &mut *(s_addr as *mut Surface);
        if !surf.view.is_null() {
            let _: () = objc2::msg_send![surf.view, removeFromSuperview];
            let _: () = objc2::msg_send![surf.view, release];
            surf.view = ptr::null_mut();
        }
        // layer is owned by the view; do not release.
        surf.layer = ptr::null_mut();
        surf.drop_input_texture();
    });

    // Reclaim the heap allocation.
    unsafe { drop(Box::from_raw(s_ptr)) };
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_surface_present(s: *mut c_void, raw_info: *const c_void) -> bool {
    if s.is_null() || raw_info.is_null() {
        return false;
    }
    let s_ptr = s as *mut Surface;
    let info = unsafe { &*(raw_info as *const cef_dll_sys::_cef_accelerated_paint_info_t) };

    // is-cef-main = bottom-of-stack check.
    let is_main = {
        let stack = G_SURFACE_STACK.lock().unwrap();
        stack.first().map(|e| e.0) == Some(s_ptr)
    };

    if is_main {
        if G_IN_TRANSITION.load(std::sync::atomic::Ordering::SeqCst) {
            return false;
        }
        unsafe { present_iosurface(&mut *s_ptr, info) };
        // Clear the expected-size gate when the incoming frame matches.
        let mut exp = G_EXPECTED_SIZE.lock().unwrap();
        if exp.0 > 0 {
            let surface = info.shared_texture_io_surface as IOSurfaceRef;
            if !surface.is_null() {
                let w = unsafe { IOSurfaceGetWidth(surface) } as c_int;
                let h = unsafe { IOSurfaceGetHeight(surface) } as c_int;
                if w == exp.0 && h == exp.1 {
                    *exp = (0, 0);
                    crate::jfn_macos_transition_clear();
                }
            }
        }
        return true;
    }
    unsafe { present_iosurface(&mut *s_ptr, info) };
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_surface_resize(
    s: *mut c_void,
    lw: c_int,
    _lh: c_int,
    pw: c_int,
    ph: c_int,
) {
    if s.is_null() {
        return;
    }
    let s_ptr = s as *mut Surface;
    let s_addr = s_ptr as usize;
    run_on_main_async(move || unsafe {
        let surf = &*(s_addr as *mut Surface);
        if surf.view.is_null() || surf.layer.is_null() {
            return;
        }
        let win = jfn_macos_get_window();
        if !win.is_null() {
            let content_view: *mut AnyObject = objc2::msg_send![win, contentView];
            if !content_view.is_null() {
                let bounds: objc2_foundation::NSRect = objc2::msg_send![content_view, bounds];
                let _: () = objc2::msg_send![surf.view, setFrame: bounds];
            }
        }
        let scale: f64 = if pw > 0 && lw > 0 {
            pw as f64 / lw as f64
        } else if !win.is_null() {
            objc2::msg_send![win, backingScaleFactor]
        } else {
            1.0
        };
        let _: () = objc2::msg_send![surf.layer, setContentsScale: scale];
        if pw > 0 && ph > 0 {
            let _: () = objc2::msg_send![
                surf.layer,
                setDrawableSize: CGSize { width: pw as f64, height: ph as f64 }
            ];
        }
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_surface_set_visible(s: *mut c_void, visible: bool) {
    if s.is_null() {
        return;
    }
    let s_addr = s as usize;
    run_on_main_async(move || unsafe {
        let surf = &*(s_addr as *mut Surface);
        if !surf.view.is_null() {
            let _: () = objc2::msg_send![surf.view, setHidden: !visible];
        }
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_restack(ordered: *const *mut c_void, n: usize) {
    // Copy the order into a Vec<usize> we can move into the closure.
    let order: Vec<usize> = if ordered.is_null() || n == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(ordered, n) }
            .iter()
            .map(|p| *p as usize)
            .collect()
    };

    let apply = move || unsafe {
        {
            let mut stack = G_SURFACE_STACK.lock().unwrap();
            stack.clear();
            stack.extend(order.iter().map(|p| StackEntry(*p as *mut Surface)));
        }
        let win = jfn_macos_get_window();
        if win.is_null() {
            return;
        }
        let content_view: *mut AnyObject = objc2::msg_send![win, contentView];
        if content_view.is_null() {
            return;
        }
        let mut prev: *mut AnyObject = ptr::null_mut();
        for raw in &order {
            let s_ptr = *raw as *mut Surface;
            if s_ptr.is_null() {
                continue;
            }
            let view = (*s_ptr).view;
            if view.is_null() {
                continue;
            }
            // NSWindowAbove == 1.
            let _: () = objc2::msg_send![
                content_view,
                addSubview: view,
                positioned: 1u64,
                relativeTo: prev,
            ];
            prev = view;
        }
        // Keep the input view on top of every CefLayer.
        let input_view = jfn_macos_get_input_view();
        if !input_view.is_null() {
            let _: () = objc2::msg_send![
                content_view,
                addSubview: input_view,
                positioned: 1u64,
                relativeTo: prev,
            ];
        }
    };
    // restack must complete before Browsers proceeds — use sync.
    run_on_main_sync(apply);
}

// =====================================================================
// Fade — animate CALayer.opacity from 1.0 to 0.0 over fade_sec, fire
// on_fade_start before kicking the animation, on_complete from
// CATransaction's completion block. After the animation we hide the
// view and reset opacity so a subsequent setVisible(true) shows fully
// opaque.
// =====================================================================

/// (fn, ctx, dtor) callback triple — `dtor` fires once on drop so the
/// caller's owned context (e.g. a Box) is freed exactly once across the
/// start + complete paths.
struct CallbackTriple {
    fn_ptr: Option<unsafe extern "C" fn(*mut c_void)>,
    ctx: *mut c_void,
    dtor: Option<unsafe extern "C" fn(*mut c_void)>,
}

unsafe impl Send for CallbackTriple {}

impl CallbackTriple {
    fn fire(&self) {
        if let Some(f) = self.fn_ptr {
            unsafe { f(self.ctx) };
        }
    }
}

impl Drop for CallbackTriple {
    fn drop(&mut self) {
        if let Some(d) = self.dtor {
            unsafe { d(self.ctx) };
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_fade_surface(
    s: *mut c_void,
    fade_sec: f32,
    on_fade_start: Option<unsafe extern "C" fn(*mut c_void)>,
    start_ctx: *mut c_void,
    start_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
    on_complete: Option<unsafe extern "C" fn(*mut c_void)>,
    done_ctx: *mut c_void,
    done_dtor: Option<unsafe extern "C" fn(*mut c_void)>,
) {
    let start_cb = CallbackTriple {
        fn_ptr: on_fade_start,
        ctx: start_ctx,
        dtor: start_dtor,
    };
    let done_cb = CallbackTriple {
        fn_ptr: on_complete,
        ctx: done_ctx,
        dtor: done_dtor,
    };

    // Null/empty surface: fire both callbacks inline, then drop.
    if s.is_null() {
        start_cb.fire();
        done_cb.fire();
        return;
    }
    let s_ptr = s as *mut Surface;
    let view = unsafe { (*s_ptr).view };
    let layer = unsafe { (*s_ptr).layer };
    if view.is_null() || layer.is_null() {
        start_cb.fire();
        done_cb.fire();
        return;
    }

    let s_addr = s_ptr as usize;
    let fade_dur = fade_sec as f64;
    run_on_main_async(move || unsafe {
        start_cb.fire();
        let surf = &*(s_addr as *mut Surface);
        if surf.view.is_null() || surf.layer.is_null() {
            done_cb.fire();
            return;
        }

        // CABasicAnimation animationWithKeyPath:@"opacity"
        let key = nsstring_from_str("opacity");
        let anim_cls = objc2::class!(CABasicAnimation);
        let anim: *mut AnyObject = objc2::msg_send![anim_cls, animationWithKeyPath: key];
        let _: () = objc2::msg_send![key, release];

        // fromValue / toValue / duration / removedOnCompletion=NO /
        // fillMode=kCAFillModeForwards.
        let num_cls = objc2::class!(NSNumber);
        let from: *mut AnyObject = objc2::msg_send![num_cls, numberWithDouble: 1.0f64];
        let to: *mut AnyObject = objc2::msg_send![num_cls, numberWithDouble: 0.0f64];
        let _: () = objc2::msg_send![anim, setFromValue: from];
        let _: () = objc2::msg_send![anim, setToValue: to];
        let _: () = objc2::msg_send![anim, setDuration: fade_dur];
        let _: () = objc2::msg_send![anim, setRemovedOnCompletion: false];
        let fill_mode = nsstring_from_str("forwards");
        let _: () = objc2::msg_send![anim, setFillMode: fill_mode];
        let _: () = objc2::msg_send![fill_mode, release];

        // CATransaction begin / set completion block / addAnimation /
        // commit. The completion block hides the view, resets opacity,
        // and fires done_cb.
        let tx_cls = objc2::class!(CATransaction);
        let _: () = objc2::msg_send![tx_cls, begin];

        // Move done_cb into a heap allocation that the block trampoline
        // owns. We synthesize a block by calling the same pattern AppKit
        // uses internally: NSBlockOperation isn't suitable for completion
        // blocks. Instead we use a small helper class registered at
        // crate init time.
        //
        // Trick: setCompletionBlock: needs a real ObjC block. Build one
        // via objc2's block2 facility… but to avoid a dep we use
        // dispatch_async_f from CATransaction setCompletionBlock by
        // wrapping in a private trampoline class. Simpler approach:
        // set completion block via a Rust closure using objc2's
        // `Block2` macro would need objc2-block. To keep deps minimal,
        // we instead poll via dispatch_after_f at fade_sec to fire the
        // completion logic. This matches the user-visible behaviour
        // (animation fillMode=forwards holds the final state until our
        // completion hook fires) and avoids needing a block runtime.
        //
        // Schedule the completion via dispatch_after_f. CATransaction
        // still applies the animation; we just close it out on a timer
        // synchronized with the animation duration.

        let _: () = objc2::msg_send![surf.layer, addAnimation: anim, forKey: ptr::null_mut::<AnyObject>()];
        let _: () = objc2::msg_send![tx_cls, commit];

        // dispatch_after_f at fade_sec: hide view + reset opacity + fire done.
        struct FadeDone {
            view: usize,
            layer: usize,
            done_cb: CallbackTriple,
        }
        unsafe impl Send for FadeDone {}
        let payload = Box::new(FadeDone {
            view: surf.view as usize,
            layer: surf.layer as usize,
            done_cb,
        });
        let ctx_ptr = Box::into_raw(payload) as *mut c_void;
        unsafe extern "C" fn after_trampoline(ctx: *mut c_void) {
            unsafe {
                let payload: Box<FadeDone> = Box::from_raw(ctx as *mut FadeDone);
                let view = payload.view as *mut AnyObject;
                let layer = payload.layer as *mut AnyObject;
                if !view.is_null() {
                    let _: () = objc2::msg_send![view, setHidden: true];
                }
                if !layer.is_null() {
                    let _: () = objc2::msg_send![layer, removeAllAnimations];
                    let _: () = objc2::msg_send![layer, setOpacity: 1.0f32];
                }
                payload.done_cb.fire();
                // FadeDone drops here; done_cb's dtor fires exactly once.
            }
        }
        // dispatch_time(DISPATCH_TIME_NOW, fade_sec * NSEC_PER_SEC).
        let nsec = (fade_dur * 1_000_000_000.0) as i64;
        let when = unsafe { dispatch_time(0, nsec) };
        unsafe {
            dispatch_after_f(when, dispatch_get_main_queue(), ctx_ptr, after_trampoline);
        }
    });
}

unsafe extern "C" {
    fn dispatch_time(when: u64, delta: i64) -> u64;
    fn dispatch_after_f(
        when: u64,
        queue: *mut c_void,
        ctx: *mut c_void,
        work: unsafe extern "C" fn(*mut c_void),
    );
}

// =====================================================================
// Compositor teardown — called from C++ macos_cleanup via the narrow
// jfn_macos_compositor_cleanup accessor. Drops any stragglers, releases
// Metal, clears the stack.
// =====================================================================

#[unsafe(no_mangle)]
pub extern "C" fn jfn_macos_compositor_cleanup() {
    // Detach lingering subviews + release retained AppKit objects.
    let stragglers: Vec<usize> = {
        let mut stack = G_SURFACE_STACK.lock().unwrap();
        let out = stack.iter().map(|e| e.0 as usize).collect();
        stack.clear();
        out
    };
    for raw in stragglers {
        if raw == 0 {
            continue;
        }
        unsafe {
            let surf = &mut *(raw as *mut Surface);
            if !surf.view.is_null() {
                let _: () = objc2::msg_send![surf.view, removeFromSuperview];
                let _: () = objc2::msg_send![surf.view, release];
                surf.view = ptr::null_mut();
            }
            surf.layer = ptr::null_mut();
            surf.drop_input_texture();
            // We don't own the Box — Browsers will call free_surface on
            // each surface during its own teardown, which will reclaim
            // the heap allocation. We just zero our cached AppKit refs.
        }
    }

    *G_EXPECTED_SIZE.lock().unwrap() = (0, 0);

    let mut metal = G_METAL.lock().unwrap();
    if let Some(m) = metal.take() {
        unsafe {
            let _: () = objc2::msg_send![m.pipeline, release];
            let _: () = objc2::msg_send![m.queue, release];
            let _: () = objc2::msg_send![m.device, release];
        }
    }
}

