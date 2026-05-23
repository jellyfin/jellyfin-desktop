//! Rust author of the macOS `Platform` vtable.
//!
//! Composition only — individual platform functions (compositor, NSApp
//! lifecycle, idle inhibit, clipboard, etc.) still live in
//! `src/platform/macos.mm`. They are exposed with `extern "C"` linkage so
//! this crate can populate the vtable from them by symbol name. Subsequent
//! slices replace each thunk with a native Rust implementation; the C ABI
//! at the vtable boundary stays stable.
//!
//! Returns the shared `Platform` from `jfn-platform-abi`. Layout is pinned
//! by `static_assert`s in `src/platform/platform_ops.cpp`.

#![cfg(target_os = "macos")]
#![allow(non_snake_case)]

use std::ffi::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicBool, Ordering};

pub use jfn_platform_abi::{DisplayBackend, JfnPopupRequest, JfnRect, Platform};

// =====================================================================
// External C symbols (src/platform/macos.mm + src/input/input_macos.mm)
// =====================================================================

// Stateless no-ops ported to native Rust. The C++ statics were deleted
// so the link table picks these up by symbol name.
#[unsafe(no_mangle)]
pub extern "C" fn macos_end_transition() {
    // Transition-end is detected inline by macos_surface_present when
    // an incoming frame matches g_expected_w/h; the explicit vtable
    // entry is a no-op.
}

// =====================================================================
// State-bound bodies ported to native Rust. Each reaches the AppKit
// NSWindow* through the jfn_macos_get_window() accessor (C++ still owns
// g_window for now); call paths and side-effects mirror the original.
// =====================================================================

unsafe extern "C" {
    /// C++ accessor exporting `g_window` (defined in src/platform/macos.mm).
    /// Returns a `__bridge` (non-retaining) pointer; treat as borrowed for
    /// the duration of the call. nullptr before macos_init or after cleanup.
    fn jfn_macos_get_window() -> *mut objc2::runtime::AnyObject;

    /// C++ helper that applies the rgb color to g_window + content layer.
    /// Must be invoked on the main thread. macos_set_theme_color routes
    /// to this either inline or via dispatch_async_f.
    fn jfn_macos_apply_theme_color_on_main(rgb: u32);

    // GCD entry points used to bounce onto the main queue for the theme-
    // color apply path. libdispatch ships in libSystem so no extra link
    // step is needed. dispatch_get_main_queue() is a real exported
    // function on modern macOS (10.12+), not a macro.
    fn dispatch_async_f(
        queue: *mut c_void,
        ctx: *mut c_void,
        work: unsafe extern "C" fn(*mut c_void),
    );
    fn dispatch_get_main_queue() -> *mut c_void;
}

/// Returns true if the current thread is the AppKit main thread. Avoids
/// pulling in `objc2-foundation` `MainThreadMarker` infrastructure for a
/// single check.
fn is_main_thread() -> bool {
    unsafe {
        let cls = objc2::class!(NSThread);
        let b: bool = objc2::msg_send![cls, isMainThread];
        b
    }
}

unsafe extern "C" fn theme_color_trampoline(ctx: *mut c_void) {
    let rgb = ctx as usize as u32;
    unsafe { jfn_macos_apply_theme_color_on_main(rgb) };
}

/// Tint AppKit fills behind mpv's CAMetalLayer / NSWindow root so the
/// resize-gap stale-texture window (which CLAUDE.md explicitly accepts
/// over stretching) matches mpv's own background — no visible flash.
/// Hops to the main queue when called from another thread.
#[unsafe(no_mangle)]
pub extern "C" fn macos_set_theme_color(rgb: u32) {
    if is_main_thread() {
        unsafe { jfn_macos_apply_theme_color_on_main(rgb) };
    } else {
        let ctx = rgb as usize as *mut c_void;
        unsafe { dispatch_async_f(dispatch_get_main_queue(), ctx, theme_color_trampoline) };
    }
}

// =====================================================================
// IOPMLib idle inhibit. Keeps an assertion alive across calls; level==0
// releases it. Mirrors the C++ enum: 0=None, 1=System, 2=Display.
// =====================================================================

#[allow(non_camel_case_types)]
type IOPMAssertionID = u32;
#[allow(non_camel_case_types)]
type IOPMAssertionLevel = u32;
type IOReturn = i32;

const K_IOPM_NULL_ASSERTION_ID: IOPMAssertionID = 0;
const K_IOPM_ASSERTION_LEVEL_ON: IOPMAssertionLevel = 255;

// CFStringRef is an opaque pointer.
type CFStringRef = *const c_void;

unsafe extern "C" {
    fn IOPMAssertionCreateWithName(
        assertion_type: CFStringRef,
        assertion_level: IOPMAssertionLevel,
        assertion_name: CFStringRef,
        assertion_id: *mut IOPMAssertionID,
    ) -> IOReturn;
    fn IOPMAssertionRelease(assertion_id: IOPMAssertionID) -> IOReturn;

    // IOPMLib publishes the CFString constants used here as ordinary
    // `extern const CFStringRef` symbols.
    static kIOPMAssertionTypePreventUserIdleDisplaySleep: CFStringRef;
    static kIOPMAssertionTypePreventUserIdleSystemSleep: CFStringRef;

    fn CFStringCreateWithCStringNoCopy(
        alloc: *const c_void,
        c_str: *const c_char,
        encoding: u32,
        contents_deallocator: *const c_void,
    ) -> CFStringRef;

    /// `kCFAllocatorNull` — pass as `contents_deallocator` so CF doesn't
    /// try to free our `'static` byte buffer when the CFString is released.
    static kCFAllocatorNull: *const c_void;

    fn CFRelease(cf: *const c_void);
}

const K_CF_STRING_ENCODING_UTF8: u32 = 0x0800_0100;

static G_IDLE_ASSERTION: std::sync::atomic::AtomicU32 =
    std::sync::atomic::AtomicU32::new(K_IOPM_NULL_ASSERTION_ID);

#[unsafe(no_mangle)]
pub extern "C" fn macos_set_idle_inhibit(level: c_int) {
    // Release any active assertion first (matches C++ behavior on every
    // call, not just level == None).
    let prev = G_IDLE_ASSERTION.swap(K_IOPM_NULL_ASSERTION_ID, Ordering::SeqCst);
    if prev != K_IOPM_NULL_ASSERTION_ID {
        unsafe { IOPMAssertionRelease(prev) };
    }

    // C++ enum: None=0, System=1, Display=2.
    let assertion_type: CFStringRef = match level {
        2 => unsafe { kIOPMAssertionTypePreventUserIdleDisplaySleep },
        1 => unsafe { kIOPMAssertionTypePreventUserIdleSystemSleep },
        _ => return,
    };

    // Build a CFString for the assertion name. CFSTR() is a compiler
    // intrinsic so we synthesize the equivalent via NoCopy.
    let name_bytes = b"Jellyfin Desktop media playback\0";
    let name = unsafe {
        CFStringCreateWithCStringNoCopy(
            std::ptr::null(),
            name_bytes.as_ptr() as *const c_char,
            K_CF_STRING_ENCODING_UTF8,
            kCFAllocatorNull,
        )
    };
    if name.is_null() {
        return;
    }

    let mut id: IOPMAssertionID = K_IOPM_NULL_ASSERTION_ID;
    let rc = unsafe {
        IOPMAssertionCreateWithName(assertion_type, K_IOPM_ASSERTION_LEVEL_ON, name, &mut id)
    };
    // Release our reference; IOPM retains its own copy of the name.
    unsafe { CFRelease(name) };
    if rc == 0 && id != K_IOPM_NULL_ASSERTION_ID {
        G_IDLE_ASSERTION.store(id, Ordering::SeqCst);
    }
}

// =====================================================================
// Window-bound queries. g_window stays C-owned for the moment; both
// route through the jfn_macos_get_window() accessor.
// =====================================================================

/// Backing scale factor of `g_window`'s screen. Falls back to the main
/// screen pre-window so default-geometry sizing at startup gets a real
/// value instead of 1.0.
#[unsafe(no_mangle)]
pub extern "C" fn macos_get_scale() -> f32 {
    unsafe {
        let win = jfn_macos_get_window();
        if !win.is_null() {
            let scale: f64 = objc2::msg_send![win, backingScaleFactor];
            return scale as f32;
        }
        let screen: *mut objc2::runtime::AnyObject =
            objc2::msg_send![objc2::class!(NSScreen), mainScreen];
        if !screen.is_null() {
            let scale: f64 = objc2::msg_send![screen, backingScaleFactor];
            return scale as f32;
        }
        1.0
    }
}

/// Query the saved window position in backing pixels, relative to the
/// screen's visible frame (excluding menu bar / dock), Y measured from
/// the top. Lossless round-trip with mpv's `--geometry +X+Y`.
#[unsafe(no_mangle)]
pub extern "C" fn macos_query_window_position(x: *mut c_int, y: *mut c_int) -> bool {
    unsafe {
        let win = jfn_macos_get_window();
        if win.is_null() {
            return false;
        }
        let screen: *mut objc2::runtime::AnyObject = objc2::msg_send![win, screen];
        if screen.is_null() {
            return false;
        }
        let frame: objc2_foundation::NSRect = objc2::msg_send![win, frame];
        let visible: objc2_foundation::NSRect = objc2::msg_send![screen, visibleFrame];
        let scale: f64 = objc2::msg_send![screen, backingScaleFactor];
        let lx = frame.origin.x - visible.origin.x;
        let ly = (visible.origin.y + visible.size.height)
            - (frame.origin.y + frame.size.height);
        *x = (lx * scale) as c_int;
        *y = (ly * scale) as c_int;
        true
    }
}

// =====================================================================
// Fullscreen-transition gating flag. The C++ compositor reads this on
// every frame (macos_surface_present) and clears it when an incoming
// frame matches g_expected_w/h. Set by macos_begin_transition below;
// SeqCst matches the prior plain-bool semantics with no surrounding
// ordering requirements.
// =====================================================================

pub(crate) static G_IN_TRANSITION: AtomicBool = AtomicBool::new(false);

#[unsafe(no_mangle)]
pub extern "C" fn macos_begin_transition() {
    G_IN_TRANSITION.store(true, Ordering::SeqCst);
    compositor::drop_input_textures();
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_in_transition() -> bool {
    G_IN_TRANSITION.load(Ordering::SeqCst)
}

/// Called by C++ macos_surface_present when an incoming frame matches
/// the expected post-transition size.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_macos_transition_clear() {
    G_IN_TRANSITION.store(false, Ordering::SeqCst);
}

/// Backing scale factor of the main screen. Args are unused — the C++
/// original ignored them too because a saved (x, y) in backing pixels
/// can't be unambiguously mapped to an `NSScreen` without identity
/// persistence.
#[unsafe(no_mangle)]
pub extern "C" fn macos_get_display_scale(_x: c_int, _y: c_int) -> f32 {
    unsafe {
        let screen: *mut objc2::runtime::AnyObject =
            objc2::msg_send![objc2::class!(NSScreen), mainScreen];
        if screen.is_null() {
            return 1.0;
        }
        let scale: f64 = objc2::msg_send![screen, backingScaleFactor];
        scale as f32
    }
}

/// Clamp the saved (w, h, x, y) window geometry — in backing pixels,
/// relative to the main screen's visible frame — so the window stays
/// fully on-screen. Centers any unset axis (negative input).
#[unsafe(no_mangle)]
pub extern "C" fn macos_clamp_window_geometry(
    w: *mut c_int,
    h: *mut c_int,
    x: *mut c_int,
    y: *mut c_int,
) {
    unsafe {
        let screen: *mut objc2::runtime::AnyObject =
            objc2::msg_send![objc2::class!(NSScreen), mainScreen];
        if screen.is_null() {
            return;
        }
        let visible: objc2_foundation::NSRect = objc2::msg_send![screen, visibleFrame];
        let scale: f64 = objc2::msg_send![screen, backingScaleFactor];
        let vw = (visible.size.width * scale) as c_int;
        let vh = (visible.size.height * scale) as c_int;
        if *w > vw {
            *w = vw;
        }
        if *h > vh {
            *h = vh;
        }
        // mpv's own centering misbehaves when we override --geometry's wh
        // but leave xy unset: it pre-centers against the video size and
        // doesn't re-center after applying the requested wh.
        if *x < 0 {
            *x = (vw - *w) / 2;
        }
        if *y < 0 {
            *y = (vh - *h) / 2;
        }
        if *x + *w > vw {
            *x = vw - *w;
        }
        if *y + *h > vh {
            *y = vh - *h;
        }
        if *x < 0 {
            *x = 0;
        }
        if *y < 0 {
            *y = 0;
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_surface_present_software(
    _s: *mut c_void,
    _dirty: *const JfnRect,
    _dirty_len: usize,
    _buffer: *const c_void,
    _w: c_int,
    _h: c_int,
) -> bool {
    // CEF on macOS runs hardware-accelerated (shared_texture_supported=
    // true); the software path is not exercised. Kept for API completeness.
    false
}

unsafe extern "C" {
    fn macos_early_init();
    fn macos_init(mpv: *mut c_void) -> bool;
    fn macos_cleanup();
    fn jfn_input_macos_set_cursor(t: c_int);

    /// Narrow accessor for the input NSView installed by macos_init.
    /// Rust's macos_restack re-anchors it on top of the CefLayer
    /// subviews after every reorder. Returns nullptr before macos_init.
    fn jfn_macos_get_input_view() -> *mut objc2::runtime::AnyObject;
}

// =====================================================================
// Fullscreen — thin pass-through to mpv. The actual style/state
// transitions are driven through mpv's macOS VO. We keep the no-mpv
// guard to match the original behavior.
// =====================================================================

unsafe extern "C" {
    fn jfn_mpv_handle_get() -> *mut c_void;
    fn jfn_mpv_set_fullscreen(v: bool);
    fn jfn_mpv_toggle_fullscreen();
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_set_fullscreen(fullscreen: bool) {
    if unsafe { jfn_mpv_handle_get() }.is_null() {
        return;
    }
    unsafe { jfn_mpv_set_fullscreen(fullscreen) };
}

#[unsafe(no_mangle)]
pub extern "C" fn macos_toggle_fullscreen() {
    if unsafe { jfn_mpv_handle_get() }.is_null() {
        return;
    }
    unsafe { jfn_mpv_toggle_fullscreen() };
}

// =====================================================================
// Message pump / NSApplication run loop / wake. The C++ originals
// lived in src/platform/macos.mm; ported here so the C++ side no
// longer owns any AppKit lifecycle primitives. Symbol names are kept
// (`macos_pump`, etc.) because src/platform/macos.mm still calls
// `macos_pump()` directly during `macos_init`'s wait-for-window loop;
// the link table picks up this Rust definition.
// =====================================================================

type CFRunLoopRef = *const c_void;

unsafe extern "C" {
    fn CFRunLoopRunInMode(mode: CFStringRef, seconds: f64, return_after_source_handled: i32) -> i32;
    fn CFRunLoopGetMain() -> CFRunLoopRef;
    fn CFRunLoopWakeUp(rl: CFRunLoopRef);
    static kCFRunLoopDefaultMode: CFStringRef;
    static NSDefaultRunLoopMode: *mut objc2::runtime::AnyObject;
}

/// NSEventMask is NSUInteger; NSEventMaskAny is the bit-or of all event
/// types. The canonical macro expands to `NSUIntegerMax` (all bits set).
const NS_EVENT_MASK_ANY: u64 = u64::MAX;

/// Drain pending NSEvents without blocking, then service the default
/// CFRunLoop mode for sources that don't deliver via NSEvent (e.g.
/// CEF's wake source, GCD main-queue blocks). Used during the
/// pre-CefInitialize wait-for-VO loop where we interleave with mpv
/// events and during the macos_init wait-for-window loop.
#[unsafe(no_mangle)]
pub extern "C" fn macos_pump() {
    unsafe {
        // @autoreleasepool — bracket allocations from sendEvent / event
        // delivery so AppKit temporaries don't accumulate.
        let pool: *mut objc2::runtime::AnyObject =
            objc2::msg_send![objc2::class!(NSAutoreleasePool), new];
        let app: *mut objc2::runtime::AnyObject =
            objc2::msg_send![objc2::class!(NSApplication), sharedApplication];
        let distant_past: *mut objc2::runtime::AnyObject =
            objc2::msg_send![objc2::class!(NSDate), distantPast];
        loop {
            let event: *mut objc2::runtime::AnyObject = objc2::msg_send![
                app,
                nextEventMatchingMask: NS_EVENT_MASK_ANY,
                untilDate: distant_past,
                inMode: NSDefaultRunLoopMode,
                dequeue: true,
            ];
            if event.is_null() {
                break;
            }
            let _: () = objc2::msg_send![app, sendEvent: event];
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, 0);
        let _: () = objc2::msg_send![pool, drain];
    }
}

/// Block on the NSApplication run loop. Returns when wake_main_loop
/// calls `[NSApp stop:]`. `[NSApp run]` is the canonical Cocoa main
/// loop and properly services every run-loop mode CEF and AppKit care
/// about (default, event-tracking during drag, modal panels, etc.) —
/// which a hand-rolled nextEventMatchingMask loop in
/// NSDefaultRunLoopMode does not. CFRunLoop sources installed in
/// CommonModes (CEF wake source, GCD main-queue blocks) all fire from
/// inside this call without polling.
#[unsafe(no_mangle)]
pub extern "C" fn macos_run_main_loop() {
    unsafe {
        let app: *mut objc2::runtime::AnyObject =
            objc2::msg_send![objc2::class!(NSApplication), sharedApplication];
        let _: () = objc2::msg_send![app, run];
    }
}

unsafe extern "C" fn wake_trampoline(_ctx: *mut c_void) {
    unsafe {
        let pool: *mut objc2::runtime::AnyObject =
            objc2::msg_send![objc2::class!(NSAutoreleasePool), new];
        let app: *mut objc2::runtime::AnyObject =
            objc2::msg_send![objc2::class!(NSApplication), sharedApplication];
        // -stop: marks the loop for exit on its next iteration.
        let _: () = objc2::msg_send![app, stop: std::ptr::null_mut::<objc2::runtime::AnyObject>()];
        // Sentinel applicationDefined NSEvent guarantees there *is* a
        // next iteration even if no other events arrive.
        // NSEventTypeApplicationDefined == 15.
        const NS_EVENT_TYPE_APPLICATION_DEFINED: u64 = 15;
        let zero_point = objc2_foundation::NSPoint { x: 0.0, y: 0.0 };
        let sentinel: *mut objc2::runtime::AnyObject = objc2::msg_send![
            objc2::class!(NSEvent),
            otherEventWithType: NS_EVENT_TYPE_APPLICATION_DEFINED,
            location: zero_point,
            modifierFlags: 0u64,
            timestamp: 0.0f64,
            windowNumber: 0isize,
            context: std::ptr::null_mut::<objc2::runtime::AnyObject>(),
            subtype: 0i16,
            data1: 0isize,
            data2: 0isize,
        ];
        if !sentinel.is_null() {
            let _: () = objc2::msg_send![app, postEvent: sentinel, atStart: true];
        }
        let _: () = objc2::msg_send![pool, drain];
    }
}

/// Stop the NSApplication run loop from any thread. Hops to main via
/// `dispatch_async_f` and from there calls `-stop:` plus a sentinel
/// NSEvent so the loop wakes and exits on its next iteration. The
/// trampoline carries no state — wake is fire-and-forget.
#[unsafe(no_mangle)]
pub extern "C" fn macos_wake_main_loop() {
    unsafe {
        dispatch_async_f(dispatch_get_main_queue(), std::ptr::null_mut(), wake_trampoline);
        // Belt-and-suspenders: also wake the main CFRunLoop directly in
        // case the main thread is currently in CFRunLoopRunInMode rather
        // than [NSApp run]. Harmless when [NSApp run] is active.
        CFRunLoopWakeUp(CFRunLoopGetMain());
    }
}

// =====================================================================
// Clipboard (NSPasteboard) — read only; writes go through CEF's own
// frame->Copy() path which works correctly on macOS. NSPasteboard reads
// are synchronous so the callback fires inline on the calling thread.
// =====================================================================

#[unsafe(no_mangle)]
pub extern "C" fn macos_clipboard_read_text_async(
    on_done: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
    ctx: *mut c_void,
    dtor: Option<unsafe extern "C" fn(*mut c_void)>,
) {
    // NSPasteboardTypeString is the canonical string UTI ("public.utf8-plain-text").
    let utf8_bytes = unsafe {
        let pb: *mut objc2::runtime::AnyObject =
            objc2::msg_send![objc2::class!(NSPasteboard), generalPasteboard];
        if pb.is_null() {
            None
        } else {
            // Pass the type as an NSString literal.
            let type_cstr = c"public.utf8-plain-text";
            let ns_type: *mut objc2::runtime::AnyObject = objc2::msg_send![
                objc2::class!(NSString),
                stringWithUTF8String: type_cstr.as_ptr()
            ];
            let ns: *mut objc2::runtime::AnyObject =
                objc2::msg_send![pb, stringForType: ns_type];
            if ns.is_null() {
                None
            } else {
                let utf8: *const c_char = objc2::msg_send![ns, UTF8String];
                if utf8.is_null() {
                    None
                } else {
                    let len = std::ffi::CStr::from_ptr(utf8).to_bytes().len();
                    // Copy out before NSString is potentially released by the autorelease pool.
                    let mut v = Vec::with_capacity(len);
                    v.extend_from_slice(std::slice::from_raw_parts(utf8 as *const u8, len));
                    Some(v)
                }
            }
        }
    };

    if let Some(cb) = on_done {
        let (ptr, len) = match &utf8_bytes {
            Some(v) => (v.as_ptr() as *const c_char, v.len()),
            None => (c"".as_ptr(), 0),
        };
        unsafe { cb(ctx, ptr, len) };
    }
    if let Some(d) = dtor {
        unsafe { d(ctx) };
    }
}

/// Open an external URL via NSWorkspace. utf8/len is borrowed for the
/// duration of the call.
#[unsafe(no_mangle)]
pub extern "C" fn macos_open_external_url(utf8: *const c_char, len: usize) {
    if utf8.is_null() || len == 0 {
        return;
    }
    unsafe {
        // Build an NSString from the borrowed UTF-8 bytes (NSString copies).
        let bytes = std::slice::from_raw_parts(utf8 as *const u8, len);
        let ns_str: *mut objc2::runtime::AnyObject = objc2::msg_send![
            objc2::class!(NSString),
            alloc
        ];
        let ns_str: *mut objc2::runtime::AnyObject = objc2::msg_send![
            ns_str,
            initWithBytes: bytes.as_ptr() as *const c_void,
            length: len,
            encoding: 4u64 // NSUTF8StringEncoding
        ];
        if ns_str.is_null() {
            return;
        }
        let nsurl: *mut objc2::runtime::AnyObject = objc2::msg_send![
            objc2::class!(NSURL),
            URLWithString: ns_str
        ];
        // Balance the alloc/init retain.
        let _: () = objc2::msg_send![ns_str, release];
        if nsurl.is_null() {
            return;
        }
        let ws: *mut objc2::runtime::AnyObject = objc2::msg_send![
            objc2::class!(NSWorkspace),
            sharedWorkspace
        ];
        if ws.is_null() {
            return;
        }
        let _: bool = objc2::msg_send![ws, openURL: nsurl];
    }
}

// =====================================================================
// CAMetalLayer-based per-surface compositor. Owns:
//   - the per-surface state (NSView + CAMetalLayer + cached input texture)
//   - the surface stack (bottom-to-top, set by macos_restack)
//   - the Metal device / queue / pipeline (lazy-init on first alloc)
//   - the expected-size transition gate (macos_set_expected_size /
//     transition clear-on-match in macos_surface_present)
// CEF delivers a BGRA8 IOSurface in STRAIGHT alpha via OnAcceleratedPaint;
// we sample it into a CAMetalLayer drawable with `color.rgb *= color.a`
// in the fragment shader to convert to CoreAnimation's premultiplied
// convention. CAMetalLayer.colorspace is set from the IOSurface's
// kIOSurfaceColorSpace tag (falls back to sRGB).
// =====================================================================
mod compositor;
mod popup;
use compositor::{
    macos_alloc_surface, macos_fade_surface, macos_free_surface, macos_restack,
    macos_set_expected_size, macos_surface_present, macos_surface_resize,
    macos_surface_set_visible,
};
use popup::macos_popup_show;

// =====================================================================
// Popup no-op trampolines for compositor backends that delegate popups
// to NSMenu (set in macos_popup_show). popup_present / popup_present_
// software / popup_hide are unused on macOS.
// =====================================================================

unsafe extern "C" fn popup_hide_noop(_s: *mut c_void) {}
unsafe extern "C" fn popup_present_noop(
    _s: *mut c_void,
    _info: *const c_void,
    _lw: c_int,
    _lh: c_int,
) {
}
unsafe extern "C" fn popup_present_software_noop(
    _s: *mut c_void,
    _buffer: *const c_void,
    _pw: c_int,
    _ph: c_int,
    _lw: c_int,
    _lh: c_int,
) {
}

// =====================================================================
// Factory
// =====================================================================

/// Returns the macOS `Platform` vtable by value across the C ABI.
/// Called from C++ `make_platform(DisplayBackend::macOS)` (platform.h).
#[unsafe(no_mangle)]
pub extern "C" fn make_macos_platform() -> Platform {
    Platform {
        display: DisplayBackend::MacOS,
        early_init: Some(macos_early_init),
        init: Some(macos_init),
        cleanup: Some(macos_cleanup),
        post_window_cleanup: None,
        alloc_surface: Some(macos_alloc_surface),
        free_surface: Some(macos_free_surface),
        surface_present: Some(macos_surface_present),
        surface_present_software: Some(macos_surface_present_software),
        surface_resize: Some(macos_surface_resize),
        surface_set_visible: Some(macos_surface_set_visible),
        restack: Some(macos_restack),
        fade_surface: Some(macos_fade_surface),
        popup_show: Some(macos_popup_show),
        popup_hide: Some(popup_hide_noop),
        popup_present: Some(popup_present_noop),
        popup_present_software: Some(popup_present_software_noop),
        set_fullscreen: Some(macos_set_fullscreen),
        toggle_fullscreen: Some(macos_toggle_fullscreen),
        begin_transition: Some(macos_begin_transition),
        end_transition: Some(macos_end_transition),
        in_transition: Some(macos_in_transition),
        set_expected_size: Some(macos_set_expected_size),
        get_scale: Some(macos_get_scale),
        get_display_scale: Some(macos_get_display_scale),
        query_window_position: Some(macos_query_window_position),
        clamp_window_geometry: Some(macos_clamp_window_geometry),
        pump: Some(macos_pump),
        run_main_loop: Some(macos_run_main_loop),
        wake_main_loop: Some(macos_wake_main_loop),
        set_cursor: Some(jfn_input_macos_set_cursor),
        set_idle_inhibit: Some(macos_set_idle_inhibit),
        set_theme_color: Some(macos_set_theme_color),
        shared_texture_supported: true,
        cef_ozone_platform: [0; 32],
        clipboard_read_text_async: Some(macos_clipboard_read_text_async),
        open_external_url: Some(macos_open_external_url),
    }
}
