//! macOS NSApplication lifecycle + window/display-link/menu init.
//!
//! JellyfinApplication NSApplication subclass conforming to CefAppProtocol,
//! the application menu bar (App + Edit), the CADisplayLink target that
//! drives external BeginFrame per browser, and the NSWindow.windowShouldClose:
//! swizzle that routes the WM close button into `jfn_shutdown_initiate`.

// Platform entry points take raw pointers from the C/Obj-C boundary; the
// safety contract is the boundary's, matching the wayland/x11 backends.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use parking_lot::Mutex;
use std::cell::Cell;
use std::ffi::{c_char, c_int, c_void};
use std::ptr;

use objc2::rc::Retained;
use objc2::runtime::{AnyClass, AnyObject, Bool, Sel};
use objc2::{ClassType, DefinedClass, class, define_class, extern_class, msg_send, sel};
use objc2_foundation::{NSObject, NSObjectProtocol, NSRect};

// The input NSView is created by the input module; we adopt the
// +1-retained NSView returned here; ownership moves into INPUT_VIEW.
use crate::input::jfn_input_macos_create_view;

use jfn_playback::shutdown::{jfn_shutdown_initiate, jfn_shutting_down};

use jfn_cef::browsers::jfn_browsers_send_external_begin_frame_all;
use jfn_cef::business_about::jfn_about_open;
use jfn_mpv::api::jfn_mpv_set_force_window_position;

// libdispatch / NSAutoreleasePool primitives — shared with the rest of the
// crate but re-declared here to keep this file self-contained.
unsafe extern "C" {
    fn objc_getProtocol(name: *const c_char) -> *mut AnyObject;
    fn class_addProtocol(cls: *const AnyClass, p: *mut AnyObject) -> Bool;
    fn class_getInstanceMethod(cls: *const AnyClass, sel: Sel) -> *mut c_void;
    fn method_setImplementation(method: *mut c_void, imp: *const c_void) -> *const c_void;
    fn imp_implementationWithBlock(block: *const c_void) -> *const c_void;
}

// Foundation log target for parity with C++ LOG_PLATFORM.
const LOG_TARGET: &str = "Platform";

// =====================================================================
// Global window + input view + display link state.
// =====================================================================

struct InitState {
    /// `NSWindow*` (mpv's VO window). Retained NS object pointer; we keep
    /// it as a raw pointer so the Mutex stays `Send`. Lifetime: from
    /// macos_init through macos_cleanup.
    window: *mut AnyObject,
    /// `JellyfinInputView*` (NSView subclass owned by Rust input crate).
    /// `jfn_input_macos_create_view` returns a +1-retained ref; we hold
    /// it here until cleanup releases.
    input_view: *mut AnyObject,
    /// `DisplayLinkTarget*` retained instance.
    display_link_target: *mut AnyObject,
    /// `CADisplayLink*` retained instance.
    display_link: *mut AnyObject,
    /// `JellyfinAppMenuTarget*` retained for process lifetime.
    app_menu_target: *mut AnyObject,
    /// `JellyfinWakeTarget*` retained for process lifetime — the
    /// NSWorkspace/screen-change observer that restarts the display link.
    wake_target: *mut AnyObject,
    /// `JellyfinLifecycleObserver*` retained for process lifetime —
    /// receives NSApplication hide/unhide + NSWorkspace sleep/wake.
    lifecycle_observer: *mut AnyObject,
}

unsafe impl Send for InitState {}

static INIT_STATE: Mutex<InitState> = Mutex::new(InitState {
    window: ptr::null_mut(),
    input_view: ptr::null_mut(),
    display_link_target: ptr::null_mut(),
    display_link: ptr::null_mut(),
    app_menu_target: ptr::null_mut(),
    wake_target: ptr::null_mut(),
    lifecycle_observer: ptr::null_mut(),
});

/// Returns the `NSWindow*` (non-retaining) for use by other modules.
/// Null before macos_init or after macos_cleanup.
pub fn jfn_macos_get_window() -> *mut AnyObject {
    INIT_STATE.lock().window
}

/// Returns the `JellyfinInputView*` (non-retaining) so `macos_restack`
/// can re-anchor it on top of the CefLayer subviews after a reorder.
pub fn jfn_macos_get_input_view() -> *mut AnyObject {
    INIT_STATE.lock().input_view
}

/// Logical content view size in points. Backs `JfnIngest`'s macOS-only
/// "use the OS's logical size, not osd-dimensions" branch.
pub fn jfn_macos_query_logical_content_size(w: *mut c_int, h: *mut c_int) -> bool {
    unsafe {
        let win = INIT_STATE.lock().window;
        if win.is_null() {
            return false;
        }
        let content_view: *mut AnyObject = msg_send![win, contentView];
        if content_view.is_null() {
            return false;
        }
        let bounds: NSRect = msg_send![content_view, bounds];
        *w = bounds.size.width as c_int;
        *h = bounds.size.height as c_int;
        *w > 0 && *h > 0
    }
}

// =====================================================================
// Theme color apply on main thread. Called from macos_set_theme_color
// (lib.rs) either inline (already on main) or via dispatch_async_f.
// =====================================================================

pub fn jfn_macos_apply_theme_color_on_main(rgb: u32) {
    let win = INIT_STATE.lock().window;
    unsafe { apply_theme_color_to_window(win, rgb) };
}

/// Lock-free body. Callers that already hold `INIT_STATE` must use this
/// directly with the window they own — re-entering `INIT_STATE` self-deadlocks.
unsafe fn apply_theme_color_to_window(win: *mut AnyObject, rgb: u32) {
    if win.is_null() {
        return;
    }
    unsafe {
        let r = ((rgb >> 16) & 0xff) as f64 / 255.0;
        let g = ((rgb >> 8) & 0xff) as f64 / 255.0;
        let b = (rgb & 0xff) as f64 / 255.0;
        let nscolor_cls = class!(NSColor);
        let ns: *mut AnyObject = msg_send![
            nscolor_cls,
            colorWithSRGBRed: r,
            green: g,
            blue: b,
            alpha: 1.0f64
        ];
        let _: () = msg_send![win, setBackgroundColor: ns];
        let cv: *mut AnyObject = msg_send![win, contentView];
        if !cv.is_null() {
            let layer: *mut AnyObject = msg_send![cv, layer];
            if !layer.is_null() {
                let cg: *mut c_void = msg_send![ns, CGColor];
                let _: () = msg_send![layer, setBackgroundColor: cg];
            }
        }
    }
}

// =====================================================================
// JellyfinApplication — NSApplication subclass conforming to
// CefAppProtocol. Stores the BOOL `handlingSendEvent_` in a Cell ivar.
// =====================================================================

#[derive(Default)]
struct JellyfinAppIvars {
    handling_send_event: Cell<bool>,
}

// SAFETY: NSApplication runs on the main thread; instance state is only
// touched from main.
unsafe impl Send for JellyfinAppIvars {}
unsafe impl Sync for JellyfinAppIvars {}

extern_class!(
    #[unsafe(super(NSObject))]
    #[name = "NSApplication"]
    pub struct NSApplication;
);

define_class!(
    #[unsafe(super(NSApplication))]
    #[name = "JellyfinApplication"]
    #[ivars = JellyfinAppIvars]
    struct JellyfinApplication;

    impl JellyfinApplication {
        #[unsafe(method(isHandlingSendEvent))]
        fn is_handling_send_event(&self) -> bool {
            self.ivars().handling_send_event.get()
        }

        #[unsafe(method(setHandlingSendEvent:))]
        fn set_handling_send_event(&self, v: bool) {
            self.ivars().handling_send_event.set(v);
        }

        #[unsafe(method(sendEvent:))]
        unsafe fn send_event(&self, event: *mut AnyObject) {
            self.ivars().handling_send_event.set(true);
            unsafe {
                let _: () = msg_send![super(self), sendEvent: event];
            }
            self.ivars().handling_send_event.set(false);
        }

        #[unsafe(method(terminate:))]
        unsafe fn terminate(&self, _sender: *mut AnyObject) {
            jfn_shutdown_initiate();
        }

        #[unsafe(method(handleReopenEvent:withReplyEvent:))]
        unsafe fn handle_reopen(&self, _event: *mut AnyObject, _reply: *mut AnyObject) {
            // Deminiaturize the first miniaturized window.
            unsafe {
                let ns_app: *mut AnyObject = msg_send![class!(NSApplication), sharedApplication];
                let windows: *mut AnyObject = msg_send![ns_app, windows];
                if windows.is_null() {
                    return;
                }
                let count: usize = msg_send![windows, count];
                for i in 0..count {
                    let w: *mut AnyObject = msg_send![windows, objectAtIndex: i];
                    if w.is_null() {
                        continue;
                    }
                    let mini: bool = msg_send![w, isMiniaturized];
                    if mini {
                        let _: () = msg_send![w, deminiaturize: ptr::null_mut::<AnyObject>()];
                        break;
                    }
                }
            }
        }
    }
);

/// Attach the `CefAppProtocol` protocol to the `JellyfinApplication` class
/// at runtime. The protocol is declared only in CEF's C++ headers; we
/// look it up by name via `objc_getProtocol` and add it to the class so
/// `[NSApp conformsToProtocol:@protocol(CefAppProtocol)]` is true.
fn attach_cef_app_protocol(cls: *const AnyClass) {
    unsafe {
        let name = c"CefAppProtocol";
        let proto = objc_getProtocol(name.as_ptr());
        if !proto.is_null() {
            let _ = class_addProtocol(cls, proto);
        }
    }
}

// =====================================================================
// JellyfinAppMenuTarget — wires the App menu's "About" item.
// =====================================================================

define_class!(
    #[unsafe(super(NSObject))]
    #[name = "JellyfinAppMenuTarget"]
    pub struct JellyfinAppMenuTarget;

    impl JellyfinAppMenuTarget {
        #[unsafe(method(showAbout:))]
        unsafe fn show_about(&self, _sender: *mut AnyObject) {
            jfn_about_open();
        }
    }

    unsafe impl NSObjectProtocol for JellyfinAppMenuTarget {}
);

// =====================================================================
// DisplayLinkTarget — CADisplayLink target that drives external
// BeginFrame on every browser. Fires on the main runloop at the
// display's refresh rate.
// =====================================================================

// =====================================================================
// Lifecycle observer — bridges NSApplication hide/unhide notifications
// and NSWorkspace will-sleep/did-wake notifications into the manager
// FSM (jfn_playback::lifecycle).
// =====================================================================

define_class!(
    #[unsafe(super(NSObject))]
    #[name = "JellyfinLifecycleObserver"]
    pub struct JellyfinLifecycleObserver;

    impl JellyfinLifecycleObserver {
        #[unsafe(method(appDidHide:))]
        unsafe fn app_did_hide(&self, _n: *mut AnyObject) {
            jfn_playback::lifecycle::jfn_lifecycle_set_visible(false);
        }
        #[unsafe(method(appDidUnhide:))]
        unsafe fn app_did_unhide(&self, _n: *mut AnyObject) {
            jfn_playback::lifecycle::jfn_lifecycle_set_visible(true);
        }
        #[unsafe(method(workspaceWillSleep:))]
        unsafe fn workspace_will_sleep(&self, _n: *mut AnyObject) {
            jfn_playback::lifecycle::jfn_lifecycle_suspend();
        }
        #[unsafe(method(workspaceDidWake:))]
        unsafe fn workspace_did_wake(&self, _n: *mut AnyObject) {
            jfn_playback::lifecycle::jfn_lifecycle_resume();
        }
    }

    unsafe impl NSObjectProtocol for JellyfinLifecycleObserver {}
);

unsafe fn install_lifecycle_observer() {
    unsafe {
        let observer: Retained<JellyfinLifecycleObserver> =
            msg_send![JellyfinLifecycleObserver::class(), new];
        let observer_obj: *mut AnyObject = Retained::into_raw(observer) as *mut AnyObject;

        // App-level hide / unhide arrive via the default notification center.
        let nc: *mut AnyObject = msg_send![class!(NSNotificationCenter), defaultCenter];
        for (sel, name) in [
            (sel!(appDidHide:), c"NSApplicationDidHideNotification"),
            (sel!(appDidUnhide:), c"NSApplicationDidUnhideNotification"),
        ] {
            let name_ns: *mut AnyObject =
                msg_send![class!(NSString), stringWithUTF8String: name.as_ptr()];
            let _: () = msg_send![
                nc,
                addObserver: observer_obj,
                selector: sel,
                name: name_ns,
                object: std::ptr::null_mut::<AnyObject>(),
            ];
        }

        // System sleep / wake is published by the shared NSWorkspace's own
        // notification center, not the default one.
        let ws: *mut AnyObject = msg_send![class!(NSWorkspace), sharedWorkspace];
        let ws_nc: *mut AnyObject = msg_send![ws, notificationCenter];
        for (sel, name) in [
            (
                sel!(workspaceWillSleep:),
                c"NSWorkspaceWillSleepNotification",
            ),
            (sel!(workspaceDidWake:), c"NSWorkspaceDidWakeNotification"),
        ] {
            let name_ns: *mut AnyObject =
                msg_send![class!(NSString), stringWithUTF8String: name.as_ptr()];
            let _: () = msg_send![
                ws_nc,
                addObserver: observer_obj,
                selector: sel,
                name: name_ns,
                object: std::ptr::null_mut::<AnyObject>(),
            ];
        }

        INIT_STATE.lock().lifecycle_observer = observer_obj;
    }
}

define_class!(
    #[unsafe(super(NSObject))]
    #[name = "JellyfinDisplayLinkTarget"]
    pub struct JellyfinDisplayLinkTarget;

    impl JellyfinDisplayLinkTarget {
        #[unsafe(method(tick:))]
        unsafe fn tick(&self, _link: *mut AnyObject) {
            if jfn_shutting_down() {
                return;
            }
            jfn_browsers_send_external_begin_frame_all();
        }
    }

    unsafe impl NSObjectProtocol for JellyfinDisplayLinkTarget {}
);

// =====================================================================
// Display link lifecycle.
// =====================================================================

/// Build a CADisplayLink bound to `window`'s current screen and add it to
/// the main run loop. Returns `(target, link)` (both +1-retained) on
/// success, or `None` if the window has no screen / the factory failed.
/// Does NOT touch `InitState` — callers decide how to install the result,
/// which lets `restart_display_link` build the replacement before tearing
/// down the old one.
unsafe fn build_display_link(window: *mut AnyObject) -> Option<(*mut AnyObject, *mut AnyObject)> {
    unsafe {
        let target: Retained<JellyfinDisplayLinkTarget> =
            msg_send![JellyfinDisplayLinkTarget::class(), new];
        let target_obj: *mut AnyObject = Retained::into_raw(target) as *mut AnyObject;

        let screen: *mut AnyObject = msg_send![window, screen];
        if screen.is_null() {
            tracing::error!(target: LOG_TARGET, "[CVDL] window has no screen");
            let _: () = msg_send![target_obj, release];
            return None;
        }
        let sel_tick = sel!(tick:);
        let link: *mut AnyObject = msg_send![
            screen,
            displayLinkWithTarget: target_obj,
            selector: sel_tick,
        ];
        if link.is_null() {
            tracing::error!(target: LOG_TARGET, "[CVDL] displayLinkWithTarget failed");
            let _: () = msg_send![target_obj, release];
            return None;
        }
        // Retain — the result of `-displayLinkWithTarget:selector:` is
        // autoreleased.
        let _: () = msg_send![link, retain];

        // Force-unpause defensively (NSScreen's factory does NOT document
        // a default state for paused).
        let _: () = msg_send![link, setPaused: false];

        // Add to the main run loop in common modes AND default mode (belt
        // and braces — common modes should cover default, but if our
        // NSString constant doesn't match the runtime's @"kCFRunLoopCommonModes"
        // identity, default mode is a guaranteed fallback).
        let main_runloop: *mut AnyObject = msg_send![class!(NSRunLoop), mainRunLoop];
        let common_modes_name = c"kCFRunLoopCommonModes";
        let common_ns: *mut AnyObject = msg_send![
            class!(NSString),
            stringWithUTF8String: common_modes_name.as_ptr()
        ];
        let _: () = msg_send![link, addToRunLoop: main_runloop, forMode: common_ns];
        let default_modes_name = c"kCFRunLoopDefaultMode";
        let default_ns: *mut AnyObject = msg_send![
            class!(NSString),
            stringWithUTF8String: default_modes_name.as_ptr()
        ];
        let _: () = msg_send![link, addToRunLoop: main_runloop, forMode: default_ns];

        Some((target_obj, link))
    }
}

unsafe fn start_display_link(state: &mut InitState) -> bool {
    unsafe {
        match build_display_link(state.window) {
            Some((target_obj, link)) => {
                state.display_link_target = target_obj;
                state.display_link = link;
                tracing::info!(target: LOG_TARGET, "[CVDL] started");
                true
            }
            None => false,
        }
    }
}

/// Rebuild the display link against the window's current screen. The
/// CADisplayLink returned by `-[NSScreen displayLinkWithTarget:selector:]`
/// is bound to one specific display; after display sleep or a screen
/// reconfiguration that display goes away under it and the link stops
/// ticking, so external BeginFrame stalls and CEF paints nothing new — the
/// UI looks frozen and unresponsive. Build the replacement first, and only
/// swap + invalidate the old link if the new one came up, so a transiently
/// screen-less window on wake never leaves us with no link at all.
unsafe fn restart_display_link(state: &mut InitState) {
    unsafe {
        let Some((target_obj, link)) = build_display_link(state.window) else {
            tracing::error!(
                target: LOG_TARGET,
                "[CVDL] restart failed to build link; keeping existing link"
            );
            return;
        };
        let old_target = state.display_link_target;
        let old_link = state.display_link;
        state.display_link_target = target_obj;
        state.display_link = link;
        if !old_link.is_null() {
            let _: () = msg_send![old_link, invalidate];
            let _: () = msg_send![old_link, release];
        }
        if !old_target.is_null() {
            let _: () = msg_send![old_target, release];
        }
        tracing::info!(target: LOG_TARGET, "[CVDL] restarted");
    }
}

unsafe fn stop_display_link(state: &mut InitState) {
    unsafe {
        if state.display_link.is_null() {
            return;
        }
        let _: () = msg_send![state.display_link, invalidate];
        let _: () = msg_send![state.display_link, release];
        state.display_link = ptr::null_mut();
        if !state.display_link_target.is_null() {
            let _: () = msg_send![state.display_link_target, release];
            state.display_link_target = ptr::null_mut();
        }
        tracing::info!(target: LOG_TARGET, "[CVDL] stopped");
    }
}

/// Lock `INIT_STATE` and rebuild the display link. Skips while shutting
/// down (the link is being torn down) and when no window is held yet.
fn restart_display_link_locked() {
    if jfn_shutting_down() {
        return;
    }
    let mut state = INIT_STATE.lock();
    if state.window.is_null() {
        return;
    }
    unsafe { restart_display_link(&mut state) };
}

// =====================================================================
// JellyfinWakeTarget — observer for NSWorkspaceDidWakeNotification and
// NSApplicationDidChangeScreenParametersNotification. Both leave the
// screen-bound CADisplayLink stale (the display it was attached to slept
// or was reconfigured); restarting the link against the window's current
// screen resumes BeginFrame and restores a responsive UI.
// =====================================================================

define_class!(
    #[unsafe(super(NSObject))]
    #[name = "JellyfinWakeTarget"]
    pub struct JellyfinWakeTarget;

    impl JellyfinWakeTarget {
        #[unsafe(method(displayLinkNeedsRestart:))]
        unsafe fn display_link_needs_restart(&self, _note: *mut AnyObject) {
            tracing::info!(
                target: LOG_TARGET,
                "[WAKE] wake/screen-change; restarting display link"
            );
            restart_display_link_locked();
        }
    }

    unsafe impl NSObjectProtocol for JellyfinWakeTarget {}
);

/// Subscribe to the system wake and screen-reconfiguration notifications
/// that strand the CADisplayLink on a defunct display. Installed once from
/// `macos_init`; the target is retained for the process lifetime.
unsafe fn start_wake_observer(state: &mut InitState) {
    unsafe {
        let target: Retained<JellyfinWakeTarget> = msg_send![JellyfinWakeTarget::class(), new];
        let target_obj: *mut AnyObject = Retained::into_raw(target) as *mut AnyObject;
        state.wake_target = target_obj;

        let sel_restart = sel!(displayLinkNeedsRestart:);

        // Wake notifications post on NSWorkspace's own notification center.
        // System sleep (Apple menu, lid, full idle sleep) posts DidWake;
        // display-only sleep (screen off, machine awake) posts
        // ScreensDidWake instead. Either can strand the link, so observe
        // both — restarting is idempotent.
        let ws: *mut AnyObject = msg_send![class!(NSWorkspace), sharedWorkspace];
        let ws_nc: *mut AnyObject = msg_send![ws, notificationCenter];
        for name in [
            c"NSWorkspaceDidWakeNotification",
            c"NSWorkspaceScreensDidWakeNotification",
        ] {
            let name_ns: *mut AnyObject =
                msg_send![class!(NSString), stringWithUTF8String: name.as_ptr()];
            let _: () = msg_send![
                ws_nc,
                addObserver: target_obj,
                selector: sel_restart,
                name: name_ns,
                object: ptr::null_mut::<AnyObject>(),
            ];
        }

        // Screen reconfiguration (display added/removed/rearranged) posts
        // on the default center.
        let nc: *mut AnyObject = msg_send![class!(NSNotificationCenter), defaultCenter];
        let screen_name: *mut AnyObject = msg_send![
            class!(NSString),
            stringWithUTF8String: c"NSApplicationDidChangeScreenParametersNotification".as_ptr()
        ];
        let _: () = msg_send![
            nc,
            addObserver: target_obj,
            selector: sel_restart,
            name: screen_name,
            object: ptr::null_mut::<AnyObject>(),
        ];

        tracing::info!(
            target: LOG_TARGET,
            "[WAKE] subscribed to wake + screen-change notifications"
        );
    }
}

// =====================================================================
// macos_pump — re-exported from lib.rs (we need it here to drive the
// wait-for-window loop in macos_init).
// =====================================================================

use crate::macos_pump_block;
use std::time::Instant;

// =====================================================================
// macos_init — locates mpv's NSWindow, swizzles windowShouldClose:,
// installs the input view, sets up titlebar/theme, starts the display
// link.
// =====================================================================

pub fn macos_init(_mpv: *mut c_void) -> bool {
    tracing::info!(target: LOG_TARGET, "[INIT] macos_init: waiting for mpv window");

    let mut state = INIT_STATE.lock();
    unsafe {
        // Block the main run loop until mpv creates its NSWindow. mpv's
        // window-creation work runs as a dispatch on the main queue, which
        // fires a run-loop source — `macos_pump_block` wakes on that source
        // without polling. The deadline caps total wait at ~5s; each
        // iteration drains pending events, blocks until *some* source
        // fires (typically the window dispatch), then re-checks.
        let deadline = Instant::now() + std::time::Duration::from_secs(5);
        loop {
            let ns_app: *mut AnyObject = msg_send![class!(NSApplication), sharedApplication];
            let windows: *mut AnyObject = msg_send![ns_app, windows];
            if !windows.is_null() {
                let count: usize = msg_send![windows, count];
                for i in 0..count {
                    let w: *mut AnyObject = msg_send![windows, objectAtIndex: i];
                    if w.is_null() {
                        continue;
                    }
                    let visible: bool = msg_send![w, isVisible];
                    if visible {
                        // Retain — the returned NSArray entry is owned by
                        // the array; we promote to a strong ref.
                        let _: () = msg_send![w, retain];
                        state.window = w;
                        break;
                    }
                }
            }
            if !state.window.is_null() {
                break;
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                break;
            }
            macos_pump_block(remaining.as_secs_f64());
        }
        if state.window.is_null() {
            tracing::error!(target: LOG_TARGET, "[INIT] mpv did not create a window");
            return false;
        }
        tracing::info!(target: LOG_TARGET, "[INIT] macos_init: got window={:?}", state.window);

        // Swizzle windowShouldClose: on the actual window class — mpv's
        // implementation routes through MP_KEY_CLOSE_WIN which we
        // disable. Replace with a block that initiates shutdown and
        // returns NO (we tear down via jfn_shutdown_initiate, not by
        // letting AppKit close the window).
        let cls: *const AnyClass = msg_send![state.window, class];
        let method = class_getInstanceMethod(cls, sel!(windowShouldClose:));
        if !method.is_null() {
            // Build a block that calls jfn_shutdown_initiate and returns NO.
            // Block layout: { isa, flags, reserved, invoke, descriptor, ... }.
            // We use std::ops::Fn boxed by `block2` if available; lacking
            // a dep on block2 here we hand-roll a global block. Since we
            // only need one process-wide, a static block suffices.
            // The block signature for the swizzled method is
            //   BOOL block(id self, NSWindow* win).
            unsafe extern "C" fn close_block_invoke(
                _block: *mut c_void,
                _self_: *mut AnyObject,
                _win: *mut AnyObject,
            ) -> Bool {
                jfn_shutdown_initiate();
                Bool::NO
            }
            // Hand-rolled "global" block — flags = BLOCK_IS_GLOBAL (1<<28),
            // a single invoke slot. Layout matches the ABI Apple
            // documents in <Block.h>.
            #[repr(C)]
            struct GlobalBlock {
                isa: *const c_void,
                flags: i32,
                reserved: i32,
                invoke: unsafe extern "C" fn(*mut c_void, *mut AnyObject, *mut AnyObject) -> Bool,
                descriptor: *const BlockDescriptor,
            }
            #[repr(C)]
            struct BlockDescriptor {
                reserved: usize,
                size: usize,
            }
            unsafe impl Sync for GlobalBlock {}
            unsafe extern "C" {
                static _NSConcreteGlobalBlock: c_void;
            }
            static BLOCK_DESC: BlockDescriptor = BlockDescriptor {
                reserved: 0,
                size: std::mem::size_of::<GlobalBlock>(),
            };
            static BLOCK: GlobalBlock = GlobalBlock {
                isa: unsafe { &_NSConcreteGlobalBlock as *const c_void },
                flags: 1 << 28, // BLOCK_IS_GLOBAL
                reserved: 0,
                invoke: close_block_invoke,
                descriptor: &BLOCK_DESC,
            };
            let imp = imp_implementationWithBlock(&BLOCK as *const _ as *const c_void);
            method_setImplementation(method, imp);
        }

        // Clear --force-window-position so subsequent reconfigs don't
        // re-snap the window back to the saved boot position.
        jfn_mpv_set_force_window_position(false);

        // Dock icon.
        let bundle: *mut AnyObject = msg_send![class!(NSBundle), mainBundle];
        if !bundle.is_null() {
            let res_path: *mut AnyObject = msg_send![bundle, resourcePath];
            if !res_path.is_null() {
                let icon_name = c"AppIcon.icns";
                let icon_ns: *mut AnyObject = msg_send![
                    class!(NSString),
                    stringWithUTF8String: icon_name.as_ptr()
                ];
                let icon_path: *mut AnyObject = msg_send![
                    res_path,
                    stringByAppendingPathComponent: icon_ns
                ];
                if !icon_path.is_null() {
                    let icon: *mut AnyObject = msg_send![class!(NSImage), alloc];
                    let icon: *mut AnyObject = msg_send![
                        icon,
                        initWithContentsOfFile: icon_path
                    ];
                    if !icon.is_null() {
                        let ns_app: *mut AnyObject =
                            msg_send![class!(NSApplication), sharedApplication];
                        let _: () = msg_send![ns_app, setApplicationIconImage: icon];
                        let _: () = msg_send![icon, release];
                    }
                }
            }
        }

        // Transparent titlebar.
        let _: () = msg_send![state.window, setTitlebarAppearsTransparent: true];
        // NSWindowTitleHidden == 1.
        let _: () = msg_send![state.window, setTitleVisibility: 1isize];
        let mask: u64 = msg_send![state.window, styleMask];
        // NSWindowStyleMaskFullSizeContentView == 1 << 15.
        let _: () = msg_send![state.window, setStyleMask: (mask | (1u64 << 15))];

        let content_view: *mut AnyObject = msg_send![state.window, contentView];
        if !content_view.is_null() {
            let layer: *mut AnyObject = msg_send![content_view, layer];
            if layer.is_null() {
                let _: () = msg_send![content_view, setWantsLayer: true];
            }
        }

        // Cover AppKit fill before CEF delivers its first frame.
        // kBgColor = 0x101010 matches src/color/src/theme.rs.
        const K_BG_COLOR: u32 = 0x101010;
        // Apply directly — we're on the main thread here. Use the lock-free
        // helper since we already hold INIT_STATE.
        apply_theme_color_to_window(state.window, K_BG_COLOR);

        // Adopt the +1-retained NSView returned by the input module.
        state.input_view = jfn_input_macos_create_view() as *mut AnyObject;
        if !state.input_view.is_null() && !content_view.is_null() {
            let bounds: NSRect = msg_send![content_view, bounds];
            let _: () = msg_send![state.input_view, setFrame: bounds];
            // NSViewWidthSizable | NSViewHeightSizable = (1<<1) | (1<<4).
            let mask: u64 = (1u64 << 1) | (1u64 << 4);
            let _: () = msg_send![state.input_view, setAutoresizingMask: mask];
            let _: () = msg_send![content_view, addSubview: state.input_view];
        }

        let _: () = msg_send![state.window, setAcceptsMouseMovedEvents: true];
        let _: () = msg_send![state.window, makeFirstResponder: state.input_view];

        if !start_display_link(&mut state) {
            tracing::error!(target: LOG_TARGET, "[INIT] failed to start CADisplayLink");
            return false;
        }

        // Restart the link on wake / screen reconfiguration — otherwise it
        // stays bound to a display that slept and stops ticking.
        start_wake_observer(&mut state);

        tracing::info!(
            target: LOG_TARGET,
            "[INIT] Metal compositor initialized input_view={:?}",
            state.input_view
        );
        true
    }
}

// =====================================================================
// macos_cleanup — stop the display link, remove the input view, release
// retained AppKit handles, tear down the Rust-side compositor.
// =====================================================================

use crate::compositor::jfn_macos_compositor_cleanup;

pub fn macos_cleanup() {
    let mut state = INIT_STATE.lock();
    unsafe {
        stop_display_link(&mut state);

        if !state.input_view.is_null() {
            let _: () = msg_send![state.input_view, removeFromSuperview];
            let _: () = msg_send![state.input_view, release];
            state.input_view = ptr::null_mut();
        }

        jfn_macos_compositor_cleanup();

        if !state.window.is_null() {
            let _: () = msg_send![state.window, release];
            state.window = ptr::null_mut();
        }
    }
}

// =====================================================================
// macos_early_init — install JellyfinApplication as the NSApp instance,
// set the activation policy, build the App + Edit menu bar.
// =====================================================================

pub fn macos_early_init() {
    unsafe {
        // Attach CefAppProtocol to our subclass before -sharedApplication
        // is called so CEF's runtime conforms-to check passes.
        attach_cef_app_protocol(JellyfinApplication::class());

        let app_obj: *mut AnyObject = msg_send![JellyfinApplication::class(), sharedApplication];

        // Install Apple-event reopen handler (Dock click etc.).
        let ae_mgr: *mut AnyObject =
            msg_send![class!(NSAppleEventManager), sharedAppleEventManager];
        // kCoreEventClass = 'aevt', kAEReopenApplication = 'rapp'.
        const K_CORE_EVENT_CLASS: u32 = 0x61_65_76_74; // 'aevt'
        const K_AE_REOPEN_APPLICATION: u32 = 0x72_61_70_70; // 'rapp'
        let sel = sel!(handleReopenEvent:withReplyEvent:);
        let _: () = msg_send![
            ae_mgr,
            setEventHandler: app_obj,
            andSelector: sel,
            forEventClass: K_CORE_EVENT_CLASS,
            andEventID: K_AE_REOPEN_APPLICATION,
        ];

        // Subprocesses (GPU, renderer): hide from Dock and skip menu setup.
        let subproc = std::env::var_os("JELLYFIN_CEF_SUBPROCESS").is_some();
        if subproc {
            // NSApplicationActivationPolicyProhibited = 2.
            let _: () = msg_send![app_obj, setActivationPolicy: 2isize];
            return;
        }

        // NSApplicationActivationPolicyRegular = 0.
        let _: () = msg_send![app_obj, setActivationPolicy: 0isize];

        // Disable AppKit's automatic Edit-menu inserts (Dictation,
        // Character Palette) so a plain "e" can't trigger them.
        let defaults: *mut AnyObject = msg_send![class!(NSUserDefaults), standardUserDefaults];
        for k in [
            c"NSDisabledDictationMenuItem",
            c"NSDisabledCharacterPaletteMenuItem",
        ] {
            let ns: *mut AnyObject = msg_send![class!(NSString), stringWithUTF8String: k.as_ptr()];
            let _: () = msg_send![defaults, setBool: true, forKey: ns];
        }

        // Allocate the App menu target (kept alive for process lifetime
        // via INIT_STATE).
        let mt: Retained<JellyfinAppMenuTarget> = msg_send![JellyfinAppMenuTarget::class(), new];
        let mt_obj: *mut AnyObject = Retained::into_raw(mt) as *mut AnyObject;
        INIT_STATE.lock().app_menu_target = mt_obj;

        // Subscribe to NSApplication hide/unhide and NSWorkspace sleep/wake
        // so the manager FSM can drive CEF visibility on lifecycle changes.
        install_lifecycle_observer();

        // Build the menu bar.
        let menubar: *mut AnyObject = msg_send![class!(NSMenu), alloc];
        let menubar: *mut AnyObject = msg_send![menubar, init];

        // -- App menu ----------------------------------------------------
        let app_item: *mut AnyObject = msg_send![class!(NSMenuItem), alloc];
        let app_item: *mut AnyObject = msg_send![app_item, init];
        let _: () = msg_send![menubar, addItem: app_item];

        let app_menu: *mut AnyObject = msg_send![class!(NSMenu), alloc];
        let app_menu: *mut AnyObject = msg_send![app_menu, init];

        add_menu_item(
            app_menu,
            "About Jellyfin Desktop",
            sel!(showAbout:),
            "",
            Some(mt_obj),
            0,
        );
        add_separator(app_menu);
        add_menu_item(app_menu, "Hide Jellyfin Desktop", sel!(hide:), "h", None, 0);
        // NSEventModifierFlagOption | NSEventModifierFlagCommand
        // = (1 << 19) | (1 << 20).
        let opt_cmd_mask: u64 = (1u64 << 19) | (1u64 << 20);
        add_menu_item(
            app_menu,
            "Hide Others",
            sel!(hideOtherApplications:),
            "h",
            None,
            opt_cmd_mask,
        );
        add_menu_item(
            app_menu,
            "Show All",
            sel!(unhideAllApplications:),
            "",
            None,
            0,
        );
        add_separator(app_menu);
        add_menu_item(app_menu, "Quit", sel!(terminate:), "q", None, 0);

        let _: () = msg_send![app_item, setSubmenu: app_menu];
        let _: () = msg_send![app_menu, release];
        let _: () = msg_send![app_item, release];

        // -- Edit menu ---------------------------------------------------
        let edit_item: *mut AnyObject = msg_send![class!(NSMenuItem), alloc];
        let edit_item: *mut AnyObject = msg_send![edit_item, init];
        let _: () = msg_send![menubar, addItem: edit_item];

        let edit_menu: *mut AnyObject = msg_send![class!(NSMenu), alloc];
        let title_ns: *mut AnyObject =
            msg_send![class!(NSString), stringWithUTF8String: c"Edit".as_ptr()];
        let edit_menu: *mut AnyObject = msg_send![edit_menu, initWithTitle: title_ns];

        add_menu_item(edit_menu, "Undo", sel!(undo:), "z", None, 0);
        add_menu_item(edit_menu, "Redo", sel!(redo:), "Z", None, 0);
        add_separator(edit_menu);
        add_menu_item(edit_menu, "Cut", sel!(cut:), "x", None, 0);
        add_menu_item(edit_menu, "Copy", sel!(copy:), "c", None, 0);
        add_menu_item(edit_menu, "Paste", sel!(paste:), "v", None, 0);
        add_separator(edit_menu);
        add_menu_item(edit_menu, "Select All", sel!(selectAll:), "a", None, 0);

        let _: () = msg_send![edit_item, setSubmenu: edit_menu];
        let _: () = msg_send![edit_menu, release];
        let _: () = msg_send![edit_item, release];

        let _: () = msg_send![app_obj, setMainMenu: menubar];
        let _: () = msg_send![menubar, release];

        let _: () = msg_send![app_obj, activateIgnoringOtherApps: true];
    }
}

unsafe fn add_separator(menu: *mut AnyObject) {
    unsafe {
        let sep: *mut AnyObject = msg_send![class!(NSMenuItem), separatorItem];
        let _: () = msg_send![menu, addItem: sep];
    }
}

unsafe fn add_menu_item(
    menu: *mut AnyObject,
    title: &str,
    action: Sel,
    key_equiv: &str,
    target: Option<*mut AnyObject>,
    modifier_mask: u64,
) {
    unsafe {
        let title_c = std::ffi::CString::new(title).unwrap_or_default();
        let title_ns: *mut AnyObject = msg_send![
            class!(NSString),
            stringWithUTF8String: title_c.as_ptr()
        ];
        let ke_c = std::ffi::CString::new(key_equiv).unwrap_or_default();
        let ke_ns: *mut AnyObject = msg_send![
            class!(NSString),
            stringWithUTF8String: ke_c.as_ptr()
        ];
        let item: *mut AnyObject = msg_send![class!(NSMenuItem), alloc];
        let item: *mut AnyObject = msg_send![
            item,
            initWithTitle: title_ns,
            action: action,
            keyEquivalent: ke_ns,
        ];
        if let Some(t) = target {
            let _: () = msg_send![item, setTarget: t];
        }
        if modifier_mask != 0 {
            let _: () = msg_send![item, setKeyEquivalentModifierMask: modifier_mask];
        }
        let _: () = msg_send![menu, addItem: item];
        let _: () = msg_send![item, release];
    }
}
