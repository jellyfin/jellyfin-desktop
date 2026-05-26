//! macOS input — NSEvent translation + JellyfinInputView NSView subclass.
//!
//! The NSView is created by `macos_init` via the
//! `jfn_input_macos_create_view` extern "C" thunk;
//! `jfn_input_macos_set_cursor` is wired into the Platform vtable.
//!
//! Event dispatch goes through the `jfn_input_dispatch_*` extern "C"
//! entry points implemented in `src/input/src/lib.rs` (active-browser
//! lookup, hotkey classification, CEF forwarding).

use parking_lot::Mutex;
use std::ffi::{c_int, c_void};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};

use objc2::rc::Retained;
use objc2::runtime::{AnyObject, Bool, Sel};
use objc2::{AnyThread, DefinedClass, define_class, extern_class, msg_send, sel};
use objc2_foundation::{NSObject, NSPoint, NSRect, NSSize};

// =====================================================================
// NSView shim — objc2 has no built-in `NSView` binding without
// `objc2-app-kit` (heavy dep). Declare it as an extern class so
// `define_class!(super(NSView))` resolves.
// =====================================================================

extern_class!(
    #[unsafe(super(NSObject))]
    #[name = "NSView"]
    #[derive(Debug, PartialEq, Eq, Hash)]
    pub struct NSView;
);

use jfn_input::buttons::{BTN_LEFT, BTN_MIDDLE, BTN_RIGHT};
use jfn_input::scroll::ScrollAccum;
use jfn_platform_abi::cursor::*;
use jfn_platform_abi::event_flags::{
    EVENTFLAG_ALT_DOWN, EVENTFLAG_COMMAND_DOWN, EVENTFLAG_CONTROL_DOWN,
    EVENTFLAG_LEFT_MOUSE_BUTTON, EVENTFLAG_MIDDLE_MOUSE_BUTTON, EVENTFLAG_RIGHT_MOUSE_BUTTON,
    EVENTFLAG_SHIFT_DOWN,
};

// NSEventModifierFlags (NSEvent.h).
const NSEVENT_MOD_SHIFT: u64 = 1 << 17;
const NSEVENT_MOD_CONTROL: u64 = 1 << 18;
const NSEVENT_MOD_OPTION: u64 = 1 << 19;
const NSEVENT_MOD_COMMAND: u64 = 1 << 20;
const NSEVENT_MOD_CAPSLOCK: u64 = 1 << 16;

// NSEventType values used.
const NSEVENT_TYPE_KEYDOWN: u64 = 10;
const NSEVENT_TYPE_KEYUP: u64 = 11;

// NSEvent buttonNumber for "back"/"forward" side buttons.
const NS_MOUSE_BUTTON_BACK: isize = 3;
const NS_MOUSE_BUTTON_FORWARD: isize = 4;

// NSTrackingArea options (NSTrackingArea.h).
const NS_TRACKING_MOUSE_MOVED: u64 = 0x02;
const NS_TRACKING_MOUSE_ENTERED_AND_EXITED: u64 = 0x01;
const NS_TRACKING_ACTIVE_IN_KEY_WINDOW: u64 = 0x20;
const NS_TRACKING_IN_VISIBLE_RECT: u64 = 0x200;

// =====================================================================
// Cross-crate entry points
// =====================================================================

use jfn_input::{
    jfn_input_dispatch_char_sys, jfn_input_dispatch_history_nav, jfn_input_dispatch_key_full,
    jfn_input_dispatch_keyboard_focus, jfn_input_dispatch_mouse_button,
    jfn_input_dispatch_mouse_move, jfn_input_dispatch_scroll_precise,
};

unsafe extern "C" {
    static _dispatch_main_q: c_void;
    fn dispatch_async_f(
        queue: *mut c_void,
        ctx: *mut c_void,
        work: unsafe extern "C" fn(*mut c_void),
    );
}

#[inline]
fn dispatch_get_main_queue() -> *mut c_void {
    std::ptr::addr_of!(_dispatch_main_q) as *mut c_void
}

// =====================================================================
// Modifier / key translation
// =====================================================================

fn ns_to_cef_modifiers(flags: u64) -> u32 {
    let mut m = 0u32;
    if flags & NSEVENT_MOD_SHIFT != 0 {
        m |= EVENTFLAG_SHIFT_DOWN;
    }
    if flags & NSEVENT_MOD_CONTROL != 0 {
        m |= EVENTFLAG_CONTROL_DOWN;
    }
    if flags & NSEVENT_MOD_OPTION != 0 {
        m |= EVENTFLAG_ALT_DOWN;
    }
    if flags & NSEVENT_MOD_COMMAND != 0 {
        m |= EVENTFLAG_COMMAND_DOWN;
    }
    m
}

/// Windows VK code for CefKeyEvent.windows_key_code.
fn ns_keycode_to_vkey(kc: u16) -> i32 {
    match kc {
        // Letters (VK_A = 0x41 .. VK_Z = 0x5A)
        0x00 => b'A' as i32,
        0x0B => b'B' as i32,
        0x08 => b'C' as i32,
        0x02 => b'D' as i32,
        0x0E => b'E' as i32,
        0x03 => b'F' as i32,
        0x05 => b'G' as i32,
        0x04 => b'H' as i32,
        0x22 => b'I' as i32,
        0x26 => b'J' as i32,
        0x28 => b'K' as i32,
        0x25 => b'L' as i32,
        0x2E => b'M' as i32,
        0x2D => b'N' as i32,
        0x1F => b'O' as i32,
        0x23 => b'P' as i32,
        0x0C => b'Q' as i32,
        0x0F => b'R' as i32,
        0x01 => b'S' as i32,
        0x11 => b'T' as i32,
        0x20 => b'U' as i32,
        0x09 => b'V' as i32,
        0x0D => b'W' as i32,
        0x07 => b'X' as i32,
        0x10 => b'Y' as i32,
        0x06 => b'Z' as i32,
        // Digits (VK_0 = 0x30 .. VK_9 = 0x39)
        0x1D => b'0' as i32,
        0x12 => b'1' as i32,
        0x13 => b'2' as i32,
        0x14 => b'3' as i32,
        0x15 => b'4' as i32,
        0x17 => b'5' as i32,
        0x16 => b'6' as i32,
        0x1A => b'7' as i32,
        0x1C => b'8' as i32,
        0x19 => b'9' as i32,
        // Function keys (VK_F1 = 0x70 .. VK_F12 = 0x7B)
        0x7A => 0x70,
        0x78 => 0x71,
        0x63 => 0x72,
        0x76 => 0x73,
        0x60 => 0x74,
        0x61 => 0x75,
        0x62 => 0x76,
        0x64 => 0x77,
        0x65 => 0x78,
        0x6D => 0x79,
        0x67 => 0x7A,
        0x6F => 0x7B,
        // Navigation
        0x7B => 0x25,
        0x7E => 0x26,
        0x7C => 0x27,
        0x7D => 0x28,
        0x73 => 0x24,
        0x77 => 0x23,
        0x74 => 0x21,
        0x79 => 0x22,
        // Editing
        0x30 => 0x09,
        0x24 => 0x0D,
        0x35 => 0x1B,
        0x33 => 0x08,
        0x75 => 0x2E,
        0x31 => 0x20,
        0x72 => 0x2D,
        // Modifiers
        0x38 | 0x3C => 0x10,
        0x3B | 0x3E => 0x11,
        0x3A | 0x3D => 0x12,
        0x36 | 0x37 => 0x5B,
        0x39 => 0x14,
        // OEM punctuation
        0x29 => 0xBA,
        0x18 => 0xBB,
        0x2B => 0xBC,
        0x1B => 0xBD,
        0x2F => 0xBE,
        0x2C => 0xBF,
        0x32 => 0xC0,
        0x21 => 0xDB,
        0x2A => 0xDC,
        0x1E => 0xDD,
        0x27 => 0xDE,
        _ => 0,
    }
}

// =====================================================================
// Cursor state
// =====================================================================

static G_CURSOR_HIDDEN: AtomicBool = AtomicBool::new(false);
static G_MOUSE_INSIDE: AtomicBool = AtomicBool::new(false);
/// Pending cursor type from CEF. Updated from any thread; applied on main.
static G_PENDING_CURSOR: AtomicI32 = AtomicI32::new(0); // CT_POINTER
/// Mouse-button bits to OR into modifier masks (so CEF sees the buttons
/// held during drags). Touched only from the main thread.
static G_MOUSE_BUTTON_MODIFIERS: AtomicU32 = AtomicU32::new(0);

unsafe fn ns_cursor_for(ct: i32) -> *mut AnyObject {
    let cls = objc2::class!(NSCursor);
    let sel: Sel = match ct {
        CT_CROSS => sel!(crosshairCursor),
        CT_HAND => sel!(pointingHandCursor),
        CT_IBEAM => sel!(IBeamCursor),
        CT_VERTICALTEXT => sel!(IBeamCursorForVerticalLayout),
        CT_EASTRESIZE => sel!(resizeRightCursor),
        CT_WESTRESIZE => sel!(resizeLeftCursor),
        CT_NORTHRESIZE => sel!(resizeUpCursor),
        CT_SOUTHRESIZE => sel!(resizeDownCursor),
        CT_NORTHSOUTHRESIZE | CT_ROWRESIZE => sel!(resizeUpDownCursor),
        CT_EASTWESTRESIZE | CT_COLUMNRESIZE => sel!(resizeLeftRightCursor),
        CT_MOVE | CT_GRAB => sel!(openHandCursor),
        CT_GRABBING => sel!(closedHandCursor),
        CT_NODROP | CT_NOTALLOWED => sel!(operationNotAllowedCursor),
        CT_COPY => sel!(dragCopyCursor),
        CT_ALIAS => sel!(dragLinkCursor),
        CT_CONTEXTMENU => sel!(contextualMenuCursor),
        _ => sel!(arrowCursor),
    };
    unsafe { msg_send![cls, performSelector: sel] }
}

unsafe fn apply_cursor_state() {
    let pending = G_PENDING_CURSOR.load(Ordering::SeqCst);
    let inside = G_MOUSE_INSIDE.load(Ordering::SeqCst);
    let cls = objc2::class!(NSCursor);
    if pending == CT_NONE && inside {
        if !G_CURSOR_HIDDEN.load(Ordering::SeqCst) {
            let _: () = unsafe { msg_send![cls, hide] };
            G_CURSOR_HIDDEN.store(true, Ordering::SeqCst);
        }
    } else {
        if G_CURSOR_HIDDEN.load(Ordering::SeqCst) {
            let _: () = unsafe { msg_send![cls, unhide] };
            G_CURSOR_HIDDEN.store(false, Ordering::SeqCst);
        }
        if inside && pending != CT_NONE {
            let cur = unsafe { ns_cursor_for(pending) };
            if !cur.is_null() {
                let _: () = unsafe { msg_send![cur, set] };
            }
        }
    }
}

unsafe extern "C" fn cursor_trampoline(_ctx: *mut c_void) {
    unsafe { apply_cursor_state() };
}

/// Platform::set_cursor — safe to call from any thread.
pub fn jfn_input_macos_set_cursor(t: c_int) {
    G_PENDING_CURSOR.store(t, Ordering::SeqCst);
    unsafe {
        dispatch_async_f(
            dispatch_get_main_queue(),
            std::ptr::null_mut(),
            cursor_trampoline,
        );
    }
}

// =====================================================================
// Scroll accumulator — coalesces wheel/trackpad deltas onto a single
// flush per runloop cycle. All fields touched from main thread only.
// =====================================================================

static SCROLL: Mutex<ScrollAccum> = Mutex::new(ScrollAccum::new());

unsafe extern "C" fn scroll_flush_trampoline(_ctx: *mut c_void) {
    flush_scroll_accumulator();
}

fn flush_scroll_accumulator() {
    let flush = SCROLL.lock().flush();
    if let Some(f) = flush {
        jfn_input_dispatch_scroll_precise(
            f.x,
            f.y,
            f.dx,
            f.dy,
            f.mods,
            if f.precise { 1 } else { 0 },
        );
    }
}

// =====================================================================
// JellyfinInputView — transparent NSView capturing input for CEF.
// =====================================================================

#[derive(Default)]
struct ViewIvars {
    tracking_area: Mutex<Option<Retained<AnyObject>>>,
}

define_class!(
    #[unsafe(super(NSView))]
    #[name = "JellyfinInputView"]
    #[ivars = ViewIvars]
    struct InputView;

    impl InputView {
        #[unsafe(method(isFlipped))]
        fn is_flipped(&self) -> Bool { Bool::YES }

        #[unsafe(method(acceptsFirstResponder))]
        fn accepts_first_responder(&self) -> Bool { Bool::YES }

        #[unsafe(method(isOpaque))]
        fn is_opaque(&self) -> Bool { Bool::NO }

        #[unsafe(method(updateTrackingAreas))]
        fn update_tracking_areas(&self) {
            unsafe {
                let _: () = msg_send![super(self), updateTrackingAreas];
                let mut slot = self.ivars().tracking_area.lock();
                if let Some(old) = slot.take() {
                    let _: () = msg_send![self, removeTrackingArea: &*old];
                }
                let bounds: NSRect = msg_send![self, bounds];
                let cls = objc2::class!(NSTrackingArea);
                let area: *mut AnyObject = msg_send![cls, alloc];
                let opts: u64 = NS_TRACKING_MOUSE_MOVED
                    | NS_TRACKING_MOUSE_ENTERED_AND_EXITED
                    | NS_TRACKING_ACTIVE_IN_KEY_WINDOW
                    | NS_TRACKING_IN_VISIBLE_RECT;
                let area: *mut AnyObject = msg_send![
                    area,
                    initWithRect: bounds,
                    options: opts,
                    owner: self,
                    userInfo: std::ptr::null_mut::<AnyObject>(),
                ];
                if !area.is_null() {
                    let _: () = msg_send![self, addTrackingArea: area];
                    *slot = Some(Retained::from_raw(area).unwrap());
                }
            }
        }

        // ---- Mouse buttons ----
        #[unsafe(method(mouseDown:))]
        fn mouse_down(&self, event: &AnyObject) {
            dispatch_mouse_button(self, event, BTN_LEFT, true);
        }
        #[unsafe(method(mouseUp:))]
        fn mouse_up(&self, event: &AnyObject) {
            dispatch_mouse_button(self, event, BTN_LEFT, false);
        }
        #[unsafe(method(rightMouseDown:))]
        fn right_mouse_down(&self, event: &AnyObject) {
            dispatch_mouse_button(self, event, BTN_RIGHT, true);
        }
        #[unsafe(method(rightMouseUp:))]
        fn right_mouse_up(&self, event: &AnyObject) {
            dispatch_mouse_button(self, event, BTN_RIGHT, false);
        }
        #[unsafe(method(otherMouseDown:))]
        fn other_mouse_down(&self, event: &AnyObject) {
            let n: isize = unsafe { msg_send![event, buttonNumber] };
            if n == NS_MOUSE_BUTTON_BACK || n == NS_MOUSE_BUTTON_FORWARD {
                jfn_input_dispatch_history_nav(if n == NS_MOUSE_BUTTON_FORWARD { 1 } else { 0 });
                return;
            }
            dispatch_mouse_button(self, event, BTN_MIDDLE, true);
        }
        #[unsafe(method(otherMouseUp:))]
        fn other_mouse_up(&self, event: &AnyObject) {
            let n: isize = unsafe { msg_send![event, buttonNumber] };
            if n == NS_MOUSE_BUTTON_BACK || n == NS_MOUSE_BUTTON_FORWARD {
                return;
            }
            dispatch_mouse_button(self, event, BTN_MIDDLE, false);
        }

        // ---- Mouse move ----
        #[unsafe(method(mouseMoved:))]
        fn mouse_moved(&self, event: &AnyObject) { dispatch_mouse_move(self, event, false); }
        #[unsafe(method(mouseDragged:))]
        fn mouse_dragged(&self, event: &AnyObject) { dispatch_mouse_move(self, event, false); }
        #[unsafe(method(rightMouseDragged:))]
        fn right_mouse_dragged(&self, event: &AnyObject) { dispatch_mouse_move(self, event, false); }
        #[unsafe(method(otherMouseDragged:))]
        fn other_mouse_dragged(&self, event: &AnyObject) { dispatch_mouse_move(self, event, false); }
        #[unsafe(method(mouseEntered:))]
        fn mouse_entered(&self, event: &AnyObject) {
            G_MOUSE_INSIDE.store(true, Ordering::SeqCst);
            unsafe { apply_cursor_state() };
            dispatch_mouse_move(self, event, false);
        }
        #[unsafe(method(mouseExited:))]
        fn mouse_exited(&self, event: &AnyObject) {
            G_MOUSE_INSIDE.store(false, Ordering::SeqCst);
            unsafe { apply_cursor_state() };
            dispatch_mouse_move(self, event, true);
        }

        // ---- Scroll ----
        #[unsafe(method(scrollWheel:))]
        fn scroll_wheel(&self, event: &AnyObject) {
            let loc = mouse_loc_in_view(self, event);
            let precise: Bool = unsafe { msg_send![event, hasPreciseScrollingDeltas] };
            let precise = precise.as_bool();
            let (delta_x, delta_y) = unsafe {
                if precise {
                    let dx: f64 = msg_send![event, scrollingDeltaX];
                    let dy: f64 = msg_send![event, scrollingDeltaY];
                    (dx as f32, dy as f32)
                } else {
                    let dx: f64 = msg_send![event, deltaX];
                    let dy: f64 = msg_send![event, deltaY];
                    (dx as f32, dy as f32)
                }
            };
            let mods_raw: u64 = unsafe { msg_send![event, modifierFlags] };
            let sched = SCROLL.lock().accumulate(
                loc.x as i32,
                loc.y as i32,
                ns_to_cef_modifiers(mods_raw),
                precise,
                delta_x,
                delta_y,
            );
            if sched {
                unsafe {
                    dispatch_async_f(
                        dispatch_get_main_queue(),
                        std::ptr::null_mut(),
                        scroll_flush_trampoline,
                    );
                }
            }
        }

        // ---- Keyboard ----
        #[unsafe(method(keyDown:))]
        fn key_down(&self, event: &AnyObject) {
            let (vkey, mods, kc, ch, ch_nomod) = key_event_fields(event);
            jfn_input_dispatch_key_full(1, vkey, kc as i32, mods, ch, ch_nomod, 0);
            // Forward typed characters for text input — paired CHAR event only
            // for printable chars + Return.
            if ch != 0 {
                let c = ch;
                let forward = c == 0x0d
                    || (c >= 0x20 && c != 0x7f && !((0xF700..=0xF7FF).contains(&c)));
                if forward {
                    jfn_input_dispatch_char_sys(c as u32, mods, kc as u32, 0);
                }
            }
        }

        #[unsafe(method(keyUp:))]
        fn key_up(&self, event: &AnyObject) {
            let (vkey, mods, kc, ch, ch_nomod) = key_event_fields(event);
            jfn_input_dispatch_key_full(0, vkey, kc as i32, mods, ch, ch_nomod, 0);
        }

        #[unsafe(method(flagsChanged:))]
        fn flags_changed(&self, event: &AnyObject) {
            let kc: u16 = unsafe { msg_send![event, keyCode] };
            let raw_flags: u64 = unsafe { msg_send![event, modifierFlags] };
            // Match each modifier key code to its NS bit so we can derive
            // pressed-vs-released. character/unmodified_character left at 0
            // (correct for modifier-key NSEventTypeFlagsChanged path).
            let flag: u64 = match kc {
                56 | 60 => NSEVENT_MOD_SHIFT,
                59 | 62 => NSEVENT_MOD_CONTROL,
                58 | 61 => NSEVENT_MOD_OPTION,
                54 | 55 => NSEVENT_MOD_COMMAND,
                57 => NSEVENT_MOD_CAPSLOCK,
                _ => 0,
            };
            let pressed = if flag != 0 { (raw_flags & flag) != 0 } else { false };
            let vkey = ns_keycode_to_vkey(kc);
            let mods = ns_to_cef_modifiers(raw_flags);
            jfn_input_dispatch_key_full(if pressed { 1 } else { 0 }, vkey, kc as i32, mods, 0, 0, 0);
        }

        // ---- Focus ----
        #[unsafe(method(becomeFirstResponder))]
        fn become_first_responder(&self) -> Bool {
            jfn_input_dispatch_keyboard_focus(1);
            unsafe { msg_send![super(self), becomeFirstResponder] }
        }
        #[unsafe(method(resignFirstResponder))]
        fn resign_first_responder(&self) -> Bool {
            jfn_input_dispatch_keyboard_focus(0);
            unsafe { msg_send![super(self), resignFirstResponder] }
        }

        // ---- Edit menu actions ----
        // Without an Edit menu in the responder chain, AppKit never sends
        // these. Forward each to the active CEF browser's focused frame.
        #[unsafe(method(undo:))]
        fn undo_action(&self, _sender: *mut AnyObject) {
            if let Some(b) = jfn_platform_abi::browser_bridge() {
                b.undo();
            }
        }
        #[unsafe(method(redo:))]
        fn redo_action(&self, _sender: *mut AnyObject) {
            if let Some(b) = jfn_platform_abi::browser_bridge() {
                b.redo();
            }
        }
        #[unsafe(method(cut:))]
        fn cut_action(&self, _sender: *mut AnyObject) {
            if let Some(b) = jfn_platform_abi::browser_bridge() {
                b.cut();
            }
        }
        #[unsafe(method(copy:))]
        fn copy_action(&self, _sender: *mut AnyObject) {
            if let Some(b) = jfn_platform_abi::browser_bridge() {
                b.copy();
            }
        }
        #[unsafe(method(paste:))]
        fn paste_action(&self, _sender: *mut AnyObject) {
            if let Some(b) = jfn_platform_abi::browser_bridge() {
                b.paste();
            }
        }
        #[unsafe(method(selectAll:))]
        fn select_all_action(&self, _sender: *mut AnyObject) {
            if let Some(b) = jfn_platform_abi::browser_bridge() {
                b.select_all();
            }
        }
    }
);

fn mouse_loc_in_view(view: &InputView, event: &AnyObject) -> NSPoint {
    unsafe {
        let p: NSPoint = msg_send![event, locationInWindow];
        msg_send![view, convertPoint: p, fromView: std::ptr::null_mut::<AnyObject>()]
    }
}

fn dispatch_mouse_button(view: &InputView, event: &AnyObject, button_code: u32, pressed: bool) {
    let flag = match button_code {
        BTN_LEFT => EVENTFLAG_LEFT_MOUSE_BUTTON,
        BTN_RIGHT => EVENTFLAG_RIGHT_MOUSE_BUTTON,
        BTN_MIDDLE => EVENTFLAG_MIDDLE_MOUSE_BUTTON,
        _ => 0,
    };
    let prev = G_MOUSE_BUTTON_MODIFIERS.load(Ordering::SeqCst);
    let next = if pressed { prev | flag } else { prev & !flag };
    G_MOUSE_BUTTON_MODIFIERS.store(next, Ordering::SeqCst);

    let loc = mouse_loc_in_view(view, event);
    let raw_flags: u64 = unsafe { msg_send![event, modifierFlags] };
    let _click: isize = unsafe { msg_send![event, clickCount] };
    let mods = ns_to_cef_modifiers(raw_flags) | next;
    jfn_input_dispatch_mouse_button(
        button_code,
        if pressed { 1 } else { 0 },
        loc.x as i32,
        loc.y as i32,
        mods,
    );
}

fn dispatch_mouse_move(view: &InputView, event: &AnyObject, leave: bool) {
    let loc = mouse_loc_in_view(view, event);
    let raw_flags: u64 = unsafe { msg_send![event, modifierFlags] };
    let mods = ns_to_cef_modifiers(raw_flags) | G_MOUSE_BUTTON_MODIFIERS.load(Ordering::SeqCst);
    jfn_input_dispatch_mouse_move(loc.x as i32, loc.y as i32, mods, if leave { 1 } else { 0 });
}

/// Returns (windows_key_code, modifiers, native_keycode, character, unmodified_character).
fn key_event_fields(event: &AnyObject) -> (i32, u32, u16, u16, u16) {
    let kc: u16 = unsafe { msg_send![event, keyCode] };
    let raw_flags: u64 = unsafe { msg_send![event, modifierFlags] };
    let etype: u64 = unsafe { msg_send![event, type] };
    let (mut ch, mut ch_nomod) = (0u16, 0u16);
    if etype == NSEVENT_TYPE_KEYDOWN || etype == NSEVENT_TYPE_KEYUP {
        unsafe {
            let chars: *mut AnyObject = msg_send![event, characters];
            if !chars.is_null() {
                let len: usize = msg_send![chars, length];
                if len > 0 {
                    let c: u16 = msg_send![chars, characterAtIndex: 0usize];
                    ch = c;
                }
            }
            let chars_nm: *mut AnyObject = msg_send![event, charactersIgnoringModifiers];
            if !chars_nm.is_null() {
                let len: usize = msg_send![chars_nm, length];
                if len > 0 {
                    let c: u16 = msg_send![chars_nm, characterAtIndex: 0usize];
                    ch_nomod = c;
                }
            }
        }
    }
    (
        ns_keycode_to_vkey(kc),
        ns_to_cef_modifiers(raw_flags),
        kc,
        ch,
        ch_nomod,
    )
}

// =====================================================================
// Public entry — create the input NSView. Called from C++ macos_init
// after locating mpv's NSWindow.
// =====================================================================

/// Returns a +1-retained NSView pointer (transfers ownership to the
/// caller). The caller adds it to the window's content view subtree.
pub fn jfn_input_macos_create_view() -> *mut c_void {
    let zero_rect = NSRect {
        origin: NSPoint { x: 0.0, y: 0.0 },
        size: NSSize {
            width: 0.0,
            height: 0.0,
        },
    };
    let view = InputView::alloc().set_ivars(ViewIvars::default());
    let view: Retained<InputView> = unsafe { msg_send![super(view), initWithFrame: zero_rect] };
    // Retain across the FFI boundary; caller owns the +1.
    Retained::into_raw(view) as *mut c_void
}
