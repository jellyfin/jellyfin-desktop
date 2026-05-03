#include "input_macos.h"

#include "input.h"
#include "dispatch.h"
#include "logging.h"

#import <Cocoa/Cocoa.h>
#include <mach/mach_time.h>

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/internal/cef_types.h"

#include <atomic>
#include <cstdint>
#include <cmath>

static bool g_cursor_hidden = false;
static bool g_mouse_inside = false;
static cef_cursor_type_t g_pending_cursor = CT_POINTER;
static uint32_t g_mouse_button_modifiers = 0;

// Scroll accumulator — coalesces multiple trackpad/wheel events into a
// single CEF wheel event per runloop cycle. Precise (trackpad) deltas
// pass through unscaled; non-precise (mouse wheel) line deltas are
// converted to pixels at a constant ratio.
static float g_scroll_accum_x = 0.0f;
static float g_scroll_accum_y = 0.0f;
static int   g_scroll_x = 0, g_scroll_y = 0;
static uint32_t g_scroll_mods = 0;
static bool  g_scroll_precise = false;
static bool  g_scroll_pending = false;
static bool  g_scroll_flush_scheduled = false;

namespace input::macos {
namespace {

uint32_t ns_to_cef_modifiers(NSEventModifierFlags flags) {
    uint32_t m = 0;
    if (flags & NSEventModifierFlagShift)   m |= EVENTFLAG_SHIFT_DOWN;
    if (flags & NSEventModifierFlagControl) m |= EVENTFLAG_CONTROL_DOWN;
    if (flags & NSEventModifierFlagOption)  m |= EVENTFLAG_ALT_DOWN;
    if (flags & NSEventModifierFlagCommand) m |= EVENTFLAG_COMMAND_DOWN;
    return m;
}

// Maps NSEvent hardware keycodes to the common KeyCode enum.
// Adapted from Chromium's keyboard_code_conversion_mac.mm — covers the
// same subset of keys that the Wayland/Windows translators handle.
KeyCode ns_keycode_to_keycode(unsigned short kc) {
    switch (kc) {
    // Letters
    case 0x00: return KeyCode::A;
    case 0x0B: return KeyCode::B;
    case 0x08: return KeyCode::C;
    case 0x02: return KeyCode::D;
    case 0x0E: return KeyCode::E;
    case 0x03: return KeyCode::F;
    case 0x05: return KeyCode::G;
    case 0x04: return KeyCode::H;
    case 0x22: return KeyCode::I;
    case 0x26: return KeyCode::J;
    case 0x28: return KeyCode::K;
    case 0x25: return KeyCode::L;
    case 0x2E: return KeyCode::M;
    case 0x2D: return KeyCode::N;
    case 0x1F: return KeyCode::O;
    case 0x23: return KeyCode::P;
    case 0x0C: return KeyCode::Q;
    case 0x0F: return KeyCode::R;
    case 0x01: return KeyCode::S;
    case 0x11: return KeyCode::T;
    case 0x20: return KeyCode::U;
    case 0x09: return KeyCode::V;
    case 0x0D: return KeyCode::W;
    case 0x07: return KeyCode::X;
    case 0x10: return KeyCode::Y;
    case 0x06: return KeyCode::Z;
    // Digits (top row)
    case 0x1D: return KeyCode::Digit0;
    case 0x12: return KeyCode::Digit1;
    case 0x13: return KeyCode::Digit2;
    case 0x14: return KeyCode::Digit3;
    case 0x15: return KeyCode::Digit4;
    case 0x17: return KeyCode::Digit5;
    case 0x16: return KeyCode::Digit6;
    case 0x1A: return KeyCode::Digit7;
    case 0x1C: return KeyCode::Digit8;
    case 0x19: return KeyCode::Digit9;
    // Function keys
    case 0x7A: return KeyCode::F1;
    case 0x78: return KeyCode::F2;
    case 0x63: return KeyCode::F3;
    case 0x76: return KeyCode::F4;
    case 0x60: return KeyCode::F5;
    case 0x61: return KeyCode::F6;
    case 0x62: return KeyCode::F7;
    case 0x64: return KeyCode::F8;
    case 0x65: return KeyCode::F9;
    case 0x6D: return KeyCode::F10;
    case 0x67: return KeyCode::F11;
    case 0x6F: return KeyCode::F12;
    // Navigation
    case 0x7B: return KeyCode::ArrowLeft;
    case 0x7E: return KeyCode::ArrowUp;
    case 0x7C: return KeyCode::ArrowRight;
    case 0x7D: return KeyCode::ArrowDown;
    case 0x73: return KeyCode::Home;
    case 0x77: return KeyCode::End;
    case 0x74: return KeyCode::PageUp;
    case 0x79: return KeyCode::PageDown;
    // Editing
    case 0x30: return KeyCode::Tab;
    case 0x24: return KeyCode::Return;
    case 0x35: return KeyCode::Escape;
    case 0x33: return KeyCode::Backspace;
    case 0x75: return KeyCode::Delete;
    case 0x31: return KeyCode::Space;
    case 0x72: return KeyCode::Insert;
    // Modifiers
    case 0x38: case 0x3C: return KeyCode::Shift;
    case 0x3B: case 0x3E: return KeyCode::Control;
    case 0x3A: case 0x3D: return KeyCode::Alt;
    case 0x36: case 0x37: return KeyCode::Meta;
    case 0x39:            return KeyCode::CapsLock;
    default:              return KeyCode::Unknown;
    }
}

// Windows VK code for CefKeyEvent.windows_key_code. Covers the same
// subset as ns_keycode_to_keycode plus the small number of additional
// keys CEF needs for shortcut handling.
int ns_keycode_to_vkey(unsigned short kc) {
    switch (kc) {
    // Letters (VK_A = 0x41 .. VK_Z = 0x5A)
    case 0x00: return 'A';
    case 0x0B: return 'B';
    case 0x08: return 'C';
    case 0x02: return 'D';
    case 0x0E: return 'E';
    case 0x03: return 'F';
    case 0x05: return 'G';
    case 0x04: return 'H';
    case 0x22: return 'I';
    case 0x26: return 'J';
    case 0x28: return 'K';
    case 0x25: return 'L';
    case 0x2E: return 'M';
    case 0x2D: return 'N';
    case 0x1F: return 'O';
    case 0x23: return 'P';
    case 0x0C: return 'Q';
    case 0x0F: return 'R';
    case 0x01: return 'S';
    case 0x11: return 'T';
    case 0x20: return 'U';
    case 0x09: return 'V';
    case 0x0D: return 'W';
    case 0x07: return 'X';
    case 0x10: return 'Y';
    case 0x06: return 'Z';
    // Digits (VK_0 = 0x30 .. VK_9 = 0x39)
    case 0x1D: return '0';
    case 0x12: return '1';
    case 0x13: return '2';
    case 0x14: return '3';
    case 0x15: return '4';
    case 0x17: return '5';
    case 0x16: return '6';
    case 0x1A: return '7';
    case 0x1C: return '8';
    case 0x19: return '9';
    // Function keys (VK_F1 = 0x70 .. VK_F12 = 0x7B)
    case 0x7A: return 0x70;
    case 0x78: return 0x71;
    case 0x63: return 0x72;
    case 0x76: return 0x73;
    case 0x60: return 0x74;
    case 0x61: return 0x75;
    case 0x62: return 0x76;
    case 0x64: return 0x77;
    case 0x65: return 0x78;
    case 0x6D: return 0x79;
    case 0x67: return 0x7A;
    case 0x6F: return 0x7B;
    // Navigation
    case 0x7B: return 0x25;  // VK_LEFT
    case 0x7E: return 0x26;  // VK_UP
    case 0x7C: return 0x27;  // VK_RIGHT
    case 0x7D: return 0x28;  // VK_DOWN
    case 0x73: return 0x24;  // VK_HOME
    case 0x77: return 0x23;  // VK_END
    case 0x74: return 0x21;  // VK_PRIOR
    case 0x79: return 0x22;  // VK_NEXT
    // Editing
    case 0x30: return 0x09;  // VK_TAB
    case 0x24: return 0x0D;  // VK_RETURN
    case 0x35: return 0x1B;  // VK_ESCAPE
    case 0x33: return 0x08;  // VK_BACK
    case 0x75: return 0x2E;  // VK_DELETE
    case 0x31: return 0x20;  // VK_SPACE
    case 0x72: return 0x2D;  // VK_INSERT
    // Modifiers
    case 0x38: case 0x3C: return 0x10;  // VK_SHIFT
    case 0x3B: case 0x3E: return 0x11;  // VK_CONTROL
    case 0x3A: case 0x3D: return 0x12;  // VK_MENU (Alt)
    case 0x36: case 0x37: return 0x5B;  // VK_LWIN (Command)
    case 0x39:            return 0x14;  // VK_CAPITAL
    // OEM punctuation — so Chromium can derive event.key for DOM keydown
    // handlers (e.g. '>' from Shift+Period for jellyfin-web shortcuts).
    case 0x29: return 0xBA;  // VK_OEM_1    (;:)
    case 0x18: return 0xBB;  // VK_OEM_PLUS (=+)
    case 0x2B: return 0xBC;  // VK_OEM_COMMA (,<)
    case 0x1B: return 0xBD;  // VK_OEM_MINUS (-_)
    case 0x2F: return 0xBE;  // VK_OEM_PERIOD (.>)
    case 0x2C: return 0xBF;  // VK_OEM_2    (/?)
    case 0x32: return 0xC0;  // VK_OEM_3    (`~)
    case 0x21: return 0xDB;  // VK_OEM_4    ([{)
    case 0x2A: return 0xDC;  // VK_OEM_5    (\|)
    case 0x1E: return 0xDD;  // VK_OEM_6    (]})
    case 0x27: return 0xDE;  // VK_OEM_7    ('")
    default:              return 0;
    }
}

}  // namespace
}  // namespace input::macos

static NSCursor* cef_cursor_to_ns(cef_cursor_type_t type) {
    switch (type) {
    case CT_CROSS:                      return [NSCursor crosshairCursor];
    case CT_HAND:                       return [NSCursor pointingHandCursor];
    case CT_IBEAM:                      return [NSCursor IBeamCursor];
    case CT_VERTICALTEXT:               return [NSCursor IBeamCursorForVerticalLayout];
    case CT_EASTRESIZE:                 return [NSCursor resizeRightCursor];
    case CT_WESTRESIZE:                 return [NSCursor resizeLeftCursor];
    case CT_NORTHRESIZE:                return [NSCursor resizeUpCursor];
    case CT_SOUTHRESIZE:                return [NSCursor resizeDownCursor];
    case CT_NORTHSOUTHRESIZE:           return [NSCursor resizeUpDownCursor];
    case CT_EASTWESTRESIZE:             return [NSCursor resizeLeftRightCursor];
    case CT_COLUMNRESIZE:               return [NSCursor resizeLeftRightCursor];
    case CT_ROWRESIZE:                  return [NSCursor resizeUpDownCursor];
    case CT_MOVE:                       return [NSCursor openHandCursor];
    case CT_GRAB:                       return [NSCursor openHandCursor];
    case CT_GRABBING:                   return [NSCursor closedHandCursor];
    case CT_NODROP:                     return [NSCursor operationNotAllowedCursor];
    case CT_NOTALLOWED:                 return [NSCursor operationNotAllowedCursor];
    case CT_COPY:                       return [NSCursor dragCopyCursor];
    case CT_ALIAS:                      return [NSCursor dragLinkCursor];
    case CT_CONTEXTMENU:                return [NSCursor contextualMenuCursor];
    default:                            return [NSCursor arrowCursor];
    }
}

static void apply_cursor_state() {
    if (g_pending_cursor == CT_NONE && g_mouse_inside) {
        if (!g_cursor_hidden) {
            [NSCursor hide];
            g_cursor_hidden = true;
        }
    } else {
        if (g_cursor_hidden) {
            [NSCursor unhide];
            g_cursor_hidden = false;
        }
        if (g_mouse_inside && g_pending_cursor != CT_NONE)
            [cef_cursor_to_ns(g_pending_cursor) set];
    }
}

// =====================================================================
// JellyfinInputView — transparent NSView that captures input for CEF
// =====================================================================

// Populate a platform-agnostic KeyEvent from an NSEvent, including the
// character / unmodified_character fields. These MUST be set so CEF's
// TranslateWebKeyEvent on macOS builds a NSEventTypeKeyDown synthetic
// NSEvent instead of NSEventTypeFlagsChanged (which is what happens when
// both fields are 0, and is what caused keys like Backspace and Tab to
// fire their default action twice). CEF applies FilterSpecialCharacter
// on its end, so passing AppKit's raw DEL (0x7f) for Backspace etc. is
// fine — Chromium will map it to 0x08 in the WebKeyboardEvent.text field.
static void fill_key_event_from_nsevent(input::KeyEvent& e, NSEvent* event) {
    unsigned short kc = [event keyCode];
    e.code             = input::macos::ns_keycode_to_keycode(kc);
    e.windows_key_code = input::macos::ns_keycode_to_vkey(kc);
    e.modifiers        = input::macos::ns_to_cef_modifiers([event modifierFlags]);
    e.native_key_code  = kc;
    e.is_system_key    = false;
    if ([event type] == NSEventTypeKeyDown || [event type] == NSEventTypeKeyUp) {
        NSString* chars      = [event characters];
        NSString* charsNoMod = [event charactersIgnoringModifiers];
        if (chars.length > 0)      e.character            = [chars characterAtIndex:0];
        if (charsNoMod.length > 0) e.unmodified_character = [charsNoMod characterAtIndex:0];
    }
}

@interface JellyfinInputView : NSView
@property (nonatomic) NSTrackingArea* trackingArea;
@end

@implementation JellyfinInputView

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque { return NO; }

- (NSView*)hitTest:(NSPoint)point {
    return [super hitTest:point];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
        options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect)
        owner:self
        userInfo:nil];
    [self addTrackingArea:_trackingArea];
    LOG_INFO(LOG_PLATFORM, "[INPUT] updateTrackingAreas bounds={:.0f}x{:.0f}",
             self.bounds.size.width, self.bounds.size.height);
}

- (NSPoint)mouseLocInView:(NSEvent*)event {
    return [self convertPoint:[event locationInWindow] fromView:nil];
}

- (void)dispatchMouseButton:(NSEvent*)event
                     button:(input::MouseButton)button
                    pressed:(bool)pressed {
    uint32_t flag = 0;
    switch (button) {
    case input::MouseButton::Left:   flag = EVENTFLAG_LEFT_MOUSE_BUTTON; break;
    case input::MouseButton::Right:  flag = EVENTFLAG_RIGHT_MOUSE_BUTTON; break;
    case input::MouseButton::Middle: flag = EVENTFLAG_MIDDLE_MOUSE_BUTTON; break;
    }
    if (pressed) g_mouse_button_modifiers |= flag;
    else         g_mouse_button_modifiers &= ~flag;

    NSPoint loc = [self mouseLocInView:event];
    LOG_TRACE(LOG_PLATFORM, "[INPUT] mouseButton btn={} pressed={} ({:.0f},{:.0f})",
             (int)button, pressed ? 1 : 0, loc.x, loc.y);
    input::dispatch_mouse_button({
        .button      = button,
        .pressed     = pressed,
        .x           = (int)loc.x,
        .y           = (int)loc.y,
        .click_count = (int)[event clickCount],
        .modifiers   = input::macos::ns_to_cef_modifiers([event modifierFlags]) | g_mouse_button_modifiers,
    });
}

// --- Mouse events ---
- (void)mouseDown:(NSEvent*)event {
    [self dispatchMouseButton:event button:input::MouseButton::Left pressed:true];
}
- (void)mouseUp:(NSEvent*)event {
    [self dispatchMouseButton:event button:input::MouseButton::Left pressed:false];
}
- (void)rightMouseDown:(NSEvent*)event {
    [self dispatchMouseButton:event button:input::MouseButton::Right pressed:true];
}
- (void)rightMouseUp:(NSEvent*)event {
    [self dispatchMouseButton:event button:input::MouseButton::Right pressed:false];
}
// NSEvent buttonNumber values for the "back"/"forward" side buttons on
// standard 5-button mice. Apple doesn't define named constants for these.
static constexpr NSInteger kNSMouseButtonBack    = 3;
static constexpr NSInteger kNSMouseButtonForward = 4;

- (void)otherMouseDown:(NSEvent*)event {
    NSInteger n = [event buttonNumber];
    if (n == kNSMouseButtonBack || n == kNSMouseButtonForward) {
        input::dispatch_history_nav(n == kNSMouseButtonForward);
        return;
    }
    [self dispatchMouseButton:event button:input::MouseButton::Middle pressed:true];
}
- (void)otherMouseUp:(NSEvent*)event {
    NSInteger n = [event buttonNumber];
    if (n == kNSMouseButtonBack || n == kNSMouseButtonForward) return;
    [self dispatchMouseButton:event button:input::MouseButton::Middle pressed:false];
}

- (void)dispatchMouseMove:(NSEvent*)event leave:(bool)leave {
    NSPoint loc = [self mouseLocInView:event];
    input::dispatch_mouse_move({
        .x = (int)loc.x, .y = (int)loc.y,
        .modifiers = input::macos::ns_to_cef_modifiers([event modifierFlags]) | g_mouse_button_modifiers,
        .leave = leave,
    });
}

- (void)mouseMoved:(NSEvent*)event      { [self dispatchMouseMove:event leave:false]; }
- (void)mouseDragged:(NSEvent*)event    { [self dispatchMouseMove:event leave:false]; }
- (void)rightMouseDragged:(NSEvent*)event { [self dispatchMouseMove:event leave:false]; }
- (void)otherMouseDragged:(NSEvent*)event { [self dispatchMouseMove:event leave:false]; }
- (void)mouseEntered:(NSEvent*)event {
    g_mouse_inside = true;
    apply_cursor_state();
    [self dispatchMouseMove:event leave:false];
}
- (void)mouseExited:(NSEvent*)event {
    g_mouse_inside = false;
    apply_cursor_state();
    [self dispatchMouseMove:event leave:true];
}

static void flush_scroll_accumulator() {
    g_scroll_flush_scheduled = false;
    if (!g_scroll_pending) return;

    int dx = 0, dy = 0;
    if (g_scroll_precise) {
        // Precise (trackpad): pass pixel deltas straight through.
        dx = static_cast<int>(std::lround(g_scroll_accum_x));
        dy = static_cast<int>(std::lround(g_scroll_accum_y));
        g_scroll_accum_x -= dx;
        g_scroll_accum_y -= dy;
    } else {
        // Non-precise (mouse wheel): scale to pixels and drain with a
        // fractional decay so wheel events feel smooth like Chrome.
        constexpr float kDrainFraction = 0.45f;
        dx = static_cast<int>(std::lround(g_scroll_accum_x * kDrainFraction));
        dy = static_cast<int>(std::lround(g_scroll_accum_y * kDrainFraction));
        if (dx == 0 && std::fabs(g_scroll_accum_x) >= 1.0f)
            dx = g_scroll_accum_x > 0 ? 1 : -1;
        if (dy == 0 && std::fabs(g_scroll_accum_y) >= 1.0f)
            dy = g_scroll_accum_y > 0 ? 1 : -1;
        g_scroll_accum_x -= dx;
        g_scroll_accum_y -= dy;
        if (std::fabs(g_scroll_accum_x) < 0.5f) g_scroll_accum_x = 0.0f;
        if (std::fabs(g_scroll_accum_y) < 0.5f) g_scroll_accum_y = 0.0f;
    }

    g_scroll_pending = (g_scroll_accum_x != 0.0f || g_scroll_accum_y != 0.0f);
    if (dx == 0 && dy == 0) return;

    input::dispatch_scroll({
        .x = g_scroll_x, .y = g_scroll_y,
        .dx = dx, .dy = dy,
        .modifiers = g_scroll_mods,
        .precise = g_scroll_precise,
    });
}

- (void)scrollWheel:(NSEvent*)event {
    NSPoint loc = [self mouseLocInView:event];
    const bool precise = [event hasPreciseScrollingDeltas];
    float deltaX = 0.0f;
    float deltaY = 0.0f;

    if (precise) {
        deltaX = static_cast<float>([event scrollingDeltaX]);
        deltaY = static_cast<float>([event scrollingDeltaY]);
    } else {
        deltaX = static_cast<float>([event deltaX]);
        deltaY = static_cast<float>([event deltaY]);
    }

    g_scroll_x = static_cast<int>(loc.x);
    g_scroll_y = static_cast<int>(loc.y);
    g_scroll_mods = input::macos::ns_to_cef_modifiers([event modifierFlags]);
    g_scroll_precise = precise;

    if (precise) {
        g_scroll_accum_x += deltaX;
        g_scroll_accum_y += deltaY;
    } else {
        // Cocoa scrollWheel with hasPreciseScrollingDeltas == NO reports
        // line-based deltas (deltaX/Y), not pixels. Chromium maps one
        // "scroll line" to 40 CSS pixels — see
        // https://chromium.googlesource.com/chromium/blink/+/9eb0e6c/Source/web/WebInputEventFactoryMac.mm#1105
        constexpr float kPixelsPerCocoaTick = 40.0f;
        g_scroll_accum_x += deltaX * kPixelsPerCocoaTick;
        g_scroll_accum_y += deltaY * kPixelsPerCocoaTick;
    }
    g_scroll_pending = true;

    if (!g_scroll_flush_scheduled) {
        g_scroll_flush_scheduled = true;
        dispatch_async(dispatch_get_main_queue(), ^{
            flush_scroll_accumulator();
        });
    }
}

// --- Keyboard events ---
- (void)keyDown:(NSEvent*)event {
    unsigned short kc = [event keyCode];

    input::KeyEvent e{};
    fill_key_event_from_nsevent(e, event);
    e.action = input::KeyAction::Down;
    input::dispatch_key(e);

    // Forward typed characters for text input. dispatch_key above already
    // produced the raw keydown Blink needs; this CHAR event is only for
    // Blink's editor to insert printable characters into focused text
    // controls, plus Enter (where Blink needs the explicit CHAR event to
    // trigger form submission — matches the old third_party/old/src path).
    // Skip everything else — Tab, Backspace, Escape, arrow / function keys
    // — because they're already handled from the RAWKEYDOWN side and a
    // paired CHAR would produce a second editor action.
    if (e.character) {
        uint16_t c = e.character;
        bool forward = c == 0x0d /* Return */
                    || (c >= 0x20 && c != 0x7f && !(c >= 0xF700 && c <= 0xF7FF));
        if (forward) {
            input::dispatch_char(c, e.modifiers, kc, false);
        }
    }
}

- (void)keyUp:(NSEvent*)event {
    input::KeyEvent e{};
    fill_key_event_from_nsevent(e, event);
    e.action = input::KeyAction::Up;
    input::dispatch_key(e);
}

- (void)flagsChanged:(NSEvent*)event {
    unsigned short kc = [event keyCode];
    NSEventModifierFlags flag = 0;
    switch (kc) {
        case 56: case 60: flag = NSEventModifierFlagShift; break;
        case 59: case 62: flag = NSEventModifierFlagControl; break;
        case 58: case 61: flag = NSEventModifierFlagOption; break;
        case 54: case 55: flag = NSEventModifierFlagCommand; break;
        case 57: flag = NSEventModifierFlagCapsLock; break;
    }
    bool pressed = ([event modifierFlags] & flag) != 0;
    input::KeyEvent e{};
    // NSEventTypeFlagsChanged: leave character/unmodified_character at 0.
    // That is exactly the NSEventTypeFlagsChanged synthetic path CEF takes
    // when both are 0 — which is correct for modifier-key transitions.
    fill_key_event_from_nsevent(e, event);
    e.action = pressed ? input::KeyAction::Down : input::KeyAction::Up;
    input::dispatch_key(e);
}

// --- Focus ---
- (BOOL)becomeFirstResponder {
    LOG_INFO(LOG_PLATFORM, "[INPUT] becomeFirstResponder");
    input::dispatch_keyboard_focus(true);
    return [super becomeFirstResponder];
}

- (BOOL)resignFirstResponder {
    LOG_INFO(LOG_PLATFORM, "[INPUT] resignFirstResponder");
    input::dispatch_keyboard_focus(false);
    return [super resignFirstResponder];
}

// --- Edit commands ---
//
// Without an Edit menu, AppKit never sends copy:/paste:/etc. through the
// responder chain, so Cmd+C/V/X/Z/A silently do nothing.
// Forward each action to the active CEF browser's focused frame.

- (IBAction)undo:(id)sender {
    (void)sender;
    auto browser = input::active_browser();
    if (!browser) return;
    if (auto frame = browser->GetFocusedFrame()) frame->Undo();
}

- (IBAction)redo:(id)sender {
    (void)sender;
    auto browser = input::active_browser();
    if (!browser) return;
    if (auto frame = browser->GetFocusedFrame()) frame->Redo();
}

- (IBAction)cut:(id)sender {
    (void)sender;
    auto browser = input::active_browser();
    if (!browser) return;
    if (auto frame = browser->GetFocusedFrame()) frame->Cut();
}

- (IBAction)copy:(id)sender {
    (void)sender;
    auto browser = input::active_browser();
    if (!browser) return;
    if (auto frame = browser->GetFocusedFrame()) frame->Copy();
}

- (IBAction)paste:(id)sender {
    (void)sender;
    auto browser = input::active_browser();
    if (!browser) return;
    if (auto frame = browser->GetFocusedFrame()) frame->Paste();
}

- (IBAction)selectAll:(id)sender {
    (void)sender;
    auto browser = input::active_browser();
    if (!browser) return;
    if (auto frame = browser->GetFocusedFrame()) frame->SelectAll();
}

@end

namespace input::macos {

NSView* create_input_view() {
    return [[JellyfinInputView alloc] initWithFrame:NSZeroRect];
}

void set_cursor(cef_cursor_type_t type) {
    dispatch_async(dispatch_get_main_queue(), ^{
        g_pending_cursor = type;
        apply_cursor_state();
    });
}

}  // namespace input::macos
