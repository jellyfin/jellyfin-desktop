//! Wayland input layer.
//!
//! Wraps a foreign-owned wl_display (created by C++ platform_wayland), opens
//! its own EventQueue, binds wl_seat + wp_cursor_shape_manager_v1 on its own
//! registry view, and runs a dedicated input thread that polls the display
//! fd. Input events come back to C++ as primitives via JfnInputCallbacks so
//! no CEF-typed structs cross the FFI boundary.

use parking_lot::Mutex;
use std::ffi::{c_int, c_void};
use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::thread::{self, JoinHandle};

use memmap2::MmapOptions;
use wayland_backend::client::Backend;
use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::{wl_keyboard, wl_pointer, wl_registry, wl_seat, wl_surface};
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle, WEnum};
use wayland_protocols::wp::cursor_shape::v1::client::{
    wp_cursor_shape_device_v1::{self, WpCursorShapeDeviceV1},
    wp_cursor_shape_manager_v1::WpCursorShapeManagerV1,
};
use xkbcommon::xkb;

use jfn_input::buttons::{
    BTN_BACK, BTN_EXTRA, BTN_FORWARD, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE,
};
use jfn_platform_abi::event_flags::{
    EVENTFLAG_LEFT_MOUSE_BUTTON, EVENTFLAG_MIDDLE_MOUSE_BUTTON, EVENTFLAG_RIGHT_MOUSE_BUTTON,
    EVENTFLAG_SHIFT_DOWN,
};

use jfn_platform_abi::cursor::CursorShape;

const XK_MENU: u32 = 0xff67;
const XK_F10: u32 = 0xffc7;

fn is_context_menu_key(sym: u32, mods: u32) -> bool {
    sym == XK_MENU || (sym == XK_F10 && mods & EVENTFLAG_SHIFT_DOWN != 0)
}

fn cef_to_wl_shape(shape: CursorShape) -> u32 {
    use CursorShape::*;
    use wp_cursor_shape_device_v1::Shape;
    let s = match shape {
        Cross => Shape::Crosshair,
        Hand => Shape::Pointer,
        IBeam => Shape::Text,
        Wait => Shape::Wait,
        Help => Shape::Help,
        EastResize => Shape::EResize,
        NorthResize => Shape::NResize,
        NorthEastResize => Shape::NeResize,
        NorthWestResize => Shape::NwResize,
        SouthResize => Shape::SResize,
        SouthEastResize => Shape::SeResize,
        SouthWestResize => Shape::SwResize,
        WestResize => Shape::WResize,
        NorthSouthResize => Shape::NsResize,
        EastWestResize => Shape::EwResize,
        NorthEastSouthWestResize => Shape::NeswResize,
        NorthWestSouthEastResize => Shape::NwseResize,
        ColumnResize => Shape::ColResize,
        RowResize => Shape::RowResize,
        Move => Shape::Move,
        VerticalText => Shape::VerticalText,
        Cell => Shape::Cell,
        ContextMenu => Shape::ContextMenu,
        Alias => Shape::Alias,
        Progress => Shape::Progress,
        NoDrop => Shape::NoDrop,
        Copy => Shape::Copy,
        NotAllowed => Shape::NotAllowed,
        ZoomIn => Shape::ZoomIn,
        ZoomOut => Shape::ZoomOut,
        Grab => Shape::Grab,
        Grabbing => Shape::Grabbing,
        MiddlePanning | MiddlePanningVertical | MiddlePanningHorizontal => Shape::AllScroll,
        _ => Shape::Default,
    };
    s as u32
}

// Interactive move/resize requires the serial of the pointer press whose
// implicit grab drives the drag — a later key press serial would be rejected.
static LAST_BUTTON_SERIAL: AtomicU32 = AtomicU32::new(0);
// xdg_popup.grab accepts the serial of any press-type input event; tracking
// key presses too keeps the serial fresh for keyboard-opened `<select>`s
// (Enter/Space), which grab without any button press to cite.
static LAST_INPUT_SERIAL: AtomicU32 = AtomicU32::new(0);

pub fn last_button_serial() -> u32 {
    LAST_BUTTON_SERIAL.load(Ordering::Acquire)
}

pub fn last_input_serial() -> u32 {
    LAST_INPUT_SERIAL.load(Ordering::Acquire)
}

static SUPPRESSED_FOCUS_LOSS: AtomicBool = AtomicBool::new(false);
static KB_FOCUS_CB: Mutex<Option<KbFocusFn>> = Mutex::new(None);

fn suppress_focus_loss() {
    SUPPRESSED_FOCUS_LOSS.store(true, Ordering::Release);
}

fn discard_suppressed_focus_loss() {
    SUPPRESSED_FOCUS_LOSS.store(false, Ordering::Release);
}

pub(crate) fn flush_suppressed_focus_loss() {
    if SUPPRESSED_FOCUS_LOSS.swap(false, Ordering::AcqRel)
        && let Some(f) = *KB_FOCUS_CB.lock()
    {
        f(0);
    }
}

pub type MouseMoveFn = fn(x: i32, y: i32, mods: u32, leave: c_int);
pub type MouseButtonFn = fn(button: u32, pressed: c_int, x: i32, y: i32, mods: u32);
pub type ScrollFn = fn(x: i32, y: i32, dx: i32, dy: i32, mods: u32);
pub type HistoryNavFn = fn(forward: c_int);
pub type KbFocusFn = fn(gained: c_int);
pub type KeyFn = fn(keysym: u32, native_code: u32, mods: u32, pressed: c_int);
pub type CharFn = fn(codepoint: u32, mods: u32, native_code: u32);

#[derive(Clone, Copy)]
pub struct Callbacks {
    pub mouse_move: Option<MouseMoveFn>,
    pub mouse_button: Option<MouseButtonFn>,
    pub scroll: Option<ScrollFn>,
    pub history_nav: Option<HistoryNavFn>,
    pub kb_focus: Option<KbFocusFn>,
    pub key: Option<KeyFn>,
    pub char_: Option<CharFn>,
}

unsafe impl Send for Callbacks {}
unsafe impl Sync for Callbacks {}

// Safety: State is only ever accessed from the input thread after the
// worker is spawned. xkbcommon's raw pointers are not Send by default; this
// crate restricts them to the worker thread by construction.
unsafe impl Send for State {}

struct State {
    cb: Callbacks,
    // Held to keep the proxy alive while the input loop runs.
    #[allow(dead_code)]
    seat: Option<wl_seat::WlSeat>,
    pointer: Option<wl_pointer::WlPointer>,
    keyboard: Option<wl_keyboard::WlKeyboard>,
    cursor_mgr: Option<WpCursorShapeManagerV1>,
    cursor_dev: Option<WpCursorShapeDeviceV1>,

    // Pointer state.
    ptr_x: f64,
    ptr_y: f64,
    // Last pointer position on the MAIN surface. ptr_x/ptr_y rebase to
    // menu-local coords while the pointer is over the popup; events forwarded
    // to CEF during that window must use these instead.
    main_ptr_x: f64,
    main_ptr_y: f64,
    pointer_serial: u32,
    mouse_button_modifiers: u32,
    // Releases for button presses consumed by our native popup must also be
    // consumed, even if the popup closes on the press and is inactive by the
    // time Wayland delivers the matching release.
    popup_swallowed_buttons: u32,

    // Scroll accumulation across a single pointer frame.
    scroll_dx: f64,
    scroll_dy: f64,
    scroll_v120_x: i32,
    scroll_v120_y: i32,
    scroll_have_v120: bool,

    // XKB state.
    xkb_ctx: xkb::Context,
    xkb_kmap: Option<xkb::Keymap>,
    xkb_st: Option<xkb::State>,
    modifiers: u32,

    // Latest desired cursor (re-applied on pointer enter).
    cursor_type: Arc<AtomicU32>,

    menu_focus: bool,

    repeat_timer_fd: OwnedFd,
    repeat_rate: i32,
    repeat_delay: i32,
    repeat_key: Option<u32>,
}

impl State {
    fn cef_modifiers(&self) -> u32 {
        self.modifiers | self.mouse_button_modifiers
    }

    fn mouse_button_flag(button: u32) -> Option<u32> {
        match button {
            BTN_LEFT => Some(EVENTFLAG_LEFT_MOUSE_BUTTON),
            BTN_RIGHT => Some(EVENTFLAG_RIGHT_MOUSE_BUTTON),
            BTN_MIDDLE => Some(EVENTFLAG_MIDDLE_MOUSE_BUTTON),
            _ => None,
        }
    }

    fn refresh_modifiers(&mut self) {
        self.modifiers = match &self.xkb_st {
            Some(st) => jfn_linux_util::xkb::to_cef_mods(st),
            None => 0,
        };
    }

    fn apply_cursor(&mut self, qh: &QueueHandle<Self>) {
        let cef = CursorShape::from_cef(self.cursor_type.load(Ordering::Relaxed) as i32)
            .unwrap_or(CursorShape::Pointer);
        let Some(pointer) = &self.pointer else { return };
        if self.pointer_serial == 0 {
            return;
        }
        if cef == CursorShape::None {
            pointer.set_cursor(self.pointer_serial, None, 0, 0);
            return;
        }
        if self.cursor_dev.is_none()
            && let Some(mgr) = &self.cursor_mgr
        {
            self.cursor_dev = Some(mgr.get_pointer(pointer, qh, ()));
        }
        if let Some(dev) = &self.cursor_dev {
            let shape: wp_cursor_shape_device_v1::Shape = unsafe {
                std::mem::transmute::<u32, wp_cursor_shape_device_v1::Shape>(cef_to_wl_shape(cef))
            };
            dev.set_shape(self.pointer_serial, shape);
        }
    }

    fn arm_repeat(&mut self, key: u32) {
        if self.repeat_rate <= 0 {
            self.disarm_repeat();
            return;
        }
        self.repeat_key = Some(key);
        // A zero it_value disarms the timer outright regardless of
        // it_interval, so a reported delay/rate of 0 must not reach 0ms.
        let period_ms = (1000u32 / self.repeat_rate as u32).max(1);
        let spec = libc::itimerspec {
            it_interval: ms_to_timespec(period_ms),
            it_value: ms_to_timespec(self.repeat_delay.max(1) as u32),
        };
        unsafe {
            libc::timerfd_settime(
                self.repeat_timer_fd.as_raw_fd(),
                0,
                &spec,
                std::ptr::null_mut(),
            );
        }
    }

    fn disarm_repeat(&mut self) {
        self.repeat_key = None;
        let spec = libc::itimerspec {
            it_interval: libc::timespec {
                tv_sec: 0,
                tv_nsec: 0,
            },
            it_value: libc::timespec {
                tv_sec: 0,
                tv_nsec: 0,
            },
        };
        unsafe {
            libc::timerfd_settime(
                self.repeat_timer_fd.as_raw_fd(),
                0,
                &spec,
                std::ptr::null_mut(),
            );
        }
    }

    fn send_key(&self, key: u32, kc: xkb::Keycode, sym: u32, pressed: bool) {
        if let Some(f) = self.cb.key {
            f(sym, key, self.modifiers, if pressed { 1 } else { 0 });
        }
        if pressed && let Some(st) = &self.xkb_st {
            let cp = st.key_get_utf32(kc);
            if cp > 0
                && let Some(f) = self.cb.char_
            {
                f(cp, self.modifiers, key);
            }
        }
    }

    fn fire_key_repeat(&mut self) {
        let Some(key) = self.repeat_key else { return };
        // Don't leak a stale repeat into the main surface while a popup
        // has the keyboard.
        if crate::popup::active() {
            self.disarm_repeat();
            return;
        }
        let Some(st) = &self.xkb_st else { return };
        let kc: xkb::Keycode = (key + 8).into();
        let sym = st.key_get_one_sym(kc);
        self.send_key(key, kc, sym.into(), true);
    }
}

fn ms_to_timespec(ms: u32) -> libc::timespec {
    libc::timespec {
        tv_sec: (ms / 1000) as libc::time_t,
        tv_nsec: ((ms % 1000) * 1_000_000) as i64,
    }
}

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
    fn event(
        _: &mut Self,
        _: &wl_registry::WlRegistry,
        _: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_seat::WlSeat, ()> for State {
    fn event(
        state: &mut Self,
        seat: &wl_seat::WlSeat,
        event: wl_seat::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_seat::Event::Capabilities { capabilities } = event {
            let caps = match capabilities {
                WEnum::Value(c) => c,
                _ => return,
            };
            if caps.contains(wl_seat::Capability::Pointer) && state.pointer.is_none() {
                state.pointer = Some(seat.get_pointer(qh, ()));
            }
            if caps.contains(wl_seat::Capability::Keyboard) && state.keyboard.is_none() {
                state.keyboard = Some(seat.get_keyboard(qh, ()));
            }
        }
    }
}

impl Dispatch<wl_pointer::WlPointer, ()> for State {
    fn event(
        state: &mut Self,
        _: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        use wl_pointer::Event;
        match event {
            Event::Enter {
                serial,
                surface,
                surface_x,
                surface_y,
            } => {
                state.pointer_serial = serial;
                state.menu_focus = crate::popup::surface_matches(surface.id().protocol_id());
                state.ptr_x = surface_x;
                state.ptr_y = surface_y;
                if state.menu_focus {
                    crate::popup::handle_motion(surface_x as i32, surface_y as i32);
                    return;
                }
                state.main_ptr_x = surface_x;
                state.main_ptr_y = surface_y;
                state.apply_cursor(qh);
                if let Some(f) = state.cb.mouse_move {
                    f(
                        state.ptr_x as i32,
                        state.ptr_y as i32,
                        state.cef_modifiers(),
                        0,
                    );
                }
            }
            Event::Leave { .. } => {
                if state.menu_focus {
                    state.menu_focus = false;
                    return;
                }
                if let Some(f) = state.cb.mouse_move {
                    f(
                        state.ptr_x as i32,
                        state.ptr_y as i32,
                        state.cef_modifiers(),
                        1,
                    );
                }
            }
            Event::Motion {
                surface_x,
                surface_y,
                ..
            } => {
                state.ptr_x = surface_x;
                state.ptr_y = surface_y;
                if !state.menu_focus {
                    state.main_ptr_x = surface_x;
                    state.main_ptr_y = surface_y;
                }
                if crate::popup::active() {
                    if state.menu_focus {
                        crate::popup::handle_motion(surface_x as i32, surface_y as i32);
                    }
                    return;
                }
                if let Some(f) = state.cb.mouse_move {
                    f(
                        state.ptr_x as i32,
                        state.ptr_y as i32,
                        state.cef_modifiers(),
                        0,
                    );
                }
            }
            Event::Button {
                button,
                state: bs,
                serial,
                ..
            } => {
                let pressed = matches!(bs, WEnum::Value(wl_pointer::ButtonState::Pressed));
                if pressed {
                    LAST_BUTTON_SERIAL.store(serial, Ordering::Release);
                    LAST_INPUT_SERIAL.store(serial, Ordering::Release);
                }
                let flag = Self::mouse_button_flag(button);
                if crate::popup::active() {
                    if pressed {
                        if let Some(flag) = flag {
                            state.popup_swallowed_buttons |= flag;
                        }
                        if state.menu_focus {
                            crate::popup::handle_button(
                                state.ptr_x as i32,
                                state.ptr_y as i32,
                                pressed,
                            );
                        } else {
                            // Click on our own window outside the menu: the popup grab
                            // won't dismiss same-client clicks, so do it ourselves.
                            crate::popup::handle_outside_press();
                        }
                    } else if let Some(flag) = flag {
                        if state.mouse_button_modifiers & flag != 0 {
                            // This is the release for the click that opened the
                            // popup. CEF saw that press before the native menu
                            // became active, so it must also see the matching
                            // release; otherwise Blink keeps the button latched
                            // and subsequent <select> activations are ignored.
                            state.mouse_button_modifiers &= !flag;
                            if let Some(f) = state.cb.mouse_button {
                                f(
                                    button,
                                    0,
                                    state.main_ptr_x as i32,
                                    state.main_ptr_y as i32,
                                    state.cef_modifiers(),
                                );
                            }
                        } else {
                            state.popup_swallowed_buttons &= !flag;
                        }
                    }
                    return;
                }
                if let Some(flag) = flag
                    && !pressed
                    && state.popup_swallowed_buttons & flag != 0
                {
                    state.popup_swallowed_buttons &= !flag;
                    return;
                }
                if button == BTN_SIDE
                    || button == BTN_EXTRA
                    || button == BTN_BACK
                    || button == BTN_FORWARD
                {
                    if pressed {
                        let forward = button == BTN_EXTRA || button == BTN_FORWARD;
                        if let Some(f) = state.cb.history_nav {
                            f(if forward { 1 } else { 0 });
                        }
                    }
                    return;
                }
                let Some(flag) = flag else { return };
                // Grab must be requested now, while this press's implicit grab is
                // live; the menu model only arrives later via CEF's async callback.
                // Right-click arms the context menu; left-click arms a possible
                // `<select>` dropdown (CEF tells us asynchronously if one opened).
                if (button == BTN_RIGHT || button == BTN_LEFT) && pressed {
                    state.disarm_repeat();
                    crate::popup::arm(state.ptr_x as i32, state.ptr_y as i32);
                }
                if pressed {
                    state.mouse_button_modifiers |= flag;
                } else {
                    state.mouse_button_modifiers &= !flag;
                }
                if let Some(f) = state.cb.mouse_button {
                    f(
                        button,
                        if pressed { 1 } else { 0 },
                        state.ptr_x as i32,
                        state.ptr_y as i32,
                        state.cef_modifiers(),
                    );
                }
                // Drop the grab armed on the press if this click opened no menu (#494).
                if (button == BTN_RIGHT || button == BTN_LEFT)
                    && !pressed
                    && crate::popup::dismiss_if_speculative()
                {
                    // The window still holds compositor focus here — teardown
                    // returns the keyboard to the main surface, so a leave
                    // swallowed at arm time was our own grab, not a real loss.
                    discard_suppressed_focus_loss();
                }
            }
            Event::Axis { axis, value, .. } => {
                if matches!(axis, WEnum::Value(wl_pointer::Axis::VerticalScroll)) {
                    state.scroll_dy += value;
                } else {
                    state.scroll_dx += value;
                }
            }
            Event::AxisValue120 { axis, value120 } => {
                state.scroll_have_v120 = true;
                if matches!(axis, WEnum::Value(wl_pointer::Axis::VerticalScroll)) {
                    state.scroll_v120_y += value120;
                } else {
                    state.scroll_v120_x += value120;
                }
            }
            Event::AxisStop { axis, .. } => {
                if matches!(axis, WEnum::Value(wl_pointer::Axis::VerticalScroll)) {
                    state.scroll_dy = 0.0;
                } else {
                    state.scroll_dx = 0.0;
                }
            }
            Event::Frame => {
                let (mut dx, mut dy) = (0i32, 0i32);
                if state.scroll_have_v120 {
                    dx = -state.scroll_v120_x;
                    dy = -state.scroll_v120_y;
                    state.scroll_dx = 0.0;
                    state.scroll_dy = 0.0;
                } else if state.scroll_dx != 0.0 || state.scroll_dy != 0.0 {
                    let scaled_x = -state.scroll_dx * 12.0;
                    let scaled_y = -state.scroll_dy * 12.0;
                    dx = scaled_x as i32;
                    dy = scaled_y as i32;
                    state.scroll_dx = -(scaled_x - dx as f64) / 12.0;
                    state.scroll_dy = -(scaled_y - dy as f64) / 12.0;
                } else {
                    state.scroll_dx = 0.0;
                    state.scroll_dy = 0.0;
                }
                state.scroll_v120_x = 0;
                state.scroll_v120_y = 0;
                state.scroll_have_v120 = false;
                if dx == 0 && dy == 0 {
                    return;
                }
                if crate::popup::active() {
                    // Wheel must not reach CEF while a <select> popup is open —
                    // a wheel event outside Blink's popup rect cancels its
                    // widget out from under the native menu.
                    if state.menu_focus {
                        crate::popup::handle_scroll(dy);
                    }
                    return;
                }
                if let Some(f) = state.cb.scroll {
                    f(
                        state.ptr_x as i32,
                        state.ptr_y as i32,
                        dx,
                        dy,
                        state.cef_modifiers(),
                    );
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_keyboard::WlKeyboard, ()> for State {
    fn event(
        state: &mut Self,
        _: &wl_keyboard::WlKeyboard,
        event: wl_keyboard::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        use wl_keyboard::Event;
        match event {
            Event::Keymap { format, fd, size } => {
                if !matches!(format, WEnum::Value(wl_keyboard::KeymapFormat::XkbV1)) {
                    return;
                }
                let mapping = match unsafe { MmapOptions::new().len(size as usize).map(&fd) } {
                    Ok(m) => m,
                    Err(_) => return,
                };
                // map is NUL-terminated; size includes the NUL byte.
                let bytes = &mapping[..mapping.len().saturating_sub(1)];
                let s = match std::str::from_utf8(bytes) {
                    Ok(s) => s,
                    Err(_) => return,
                };
                let keymap = xkb::Keymap::new_from_string(
                    &state.xkb_ctx,
                    s.to_owned(),
                    xkb::KEYMAP_FORMAT_TEXT_V1,
                    xkb::KEYMAP_COMPILE_NO_FLAGS,
                );
                if let Some(km) = keymap {
                    let st = xkb::State::new(&km);
                    state.xkb_kmap = Some(km);
                    state.xkb_st = Some(st);
                }
            }
            Event::Enter { surface, .. } => {
                // Menu-surface enter/leave is grab plumbing, not CEF focus.
                if crate::popup::is_menu_surface(surface.id().protocol_id()) {
                    return;
                }
                discard_suppressed_focus_loss();
                if let Some(f) = state.cb.kb_focus {
                    f(1);
                }
            }
            Event::Leave { surface, .. } => {
                // Neither leave may reach CEF as focus-loss — Blink would
                // close the <select> popup the replayed selection keys still
                // need: leave of the menu surface (popup teardown), and leave
                // of the main surface caused by our own grab activating.
                if crate::popup::is_menu_surface(surface.id().protocol_id()) {
                    return;
                }
                if crate::popup::is_engaged() {
                    suppress_focus_loss();
                    return;
                }
                // Stop repeating on real focus loss, or it keeps firing
                // once focus returns to a different surface.
                state.disarm_repeat();
                if let Some(f) = state.cb.kb_focus {
                    f(0);
                }
            }
            Event::Key {
                key,
                state: ks,
                serial,
                ..
            } => {
                let pressed = matches!(ks, WEnum::Value(wl_keyboard::KeyState::Pressed));
                if pressed {
                    LAST_INPUT_SERIAL.store(serial, Ordering::Release);
                }
                let Some(st) = &state.xkb_st else { return };
                let kc: xkb::Keycode = (key + 8).into();
                let sym = st.key_get_one_sym(kc);
                if crate::popup::active() {
                    // Otherwise a repeat released here stays armed and
                    // outlives the popup.
                    if !pressed && state.repeat_key == Some(key) {
                        state.disarm_repeat();
                    }
                    crate::popup::handle_key(sym.into(), pressed);
                    return;
                }
                if pressed && is_context_menu_key(sym.into(), state.modifiers) {
                    // popup::active() only flips true once the async
                    // configure lands, so disarm now rather than rely on it.
                    state.disarm_repeat();
                    crate::popup::arm(state.ptr_x as i32, state.ptr_y as i32);
                }
                state.send_key(key, kc, sym.into(), pressed);

                let repeats = state
                    .xkb_kmap
                    .as_ref()
                    .map(|km| km.key_repeats(kc))
                    .unwrap_or(false);
                if pressed && repeats {
                    state.arm_repeat(key);
                } else if !pressed && state.repeat_key == Some(key) {
                    state.disarm_repeat();
                }
            }
            Event::Modifiers {
                mods_depressed,
                mods_latched,
                mods_locked,
                group,
                ..
            } => {
                if let Some(st) = state.xkb_st.as_mut() {
                    st.update_mask(mods_depressed, mods_latched, mods_locked, 0, 0, group);
                }
                state.refresh_modifiers();
            }
            Event::RepeatInfo { rate, delay } => {
                state.repeat_rate = rate;
                state.repeat_delay = delay;
            }
            _ => {}
        }
    }
}

impl Dispatch<WpCursorShapeManagerV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &WpCursorShapeManagerV1,
        _: <WpCursorShapeManagerV1 as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<WpCursorShapeDeviceV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &WpCursorShapeDeviceV1,
        _: <WpCursorShapeDeviceV1 as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_surface::WlSurface, ()> for State {
    fn event(
        _: &mut Self,
        _: &wl_surface::WlSurface,
        _: wl_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

pub struct InputThread {
    cursor_type: Arc<AtomicU32>,
    set_cursor_inbox: Arc<AtomicBool>,
    stop: Arc<AtomicBool>,
    wake: Arc<jfn_wake_event::WakeEvent>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

fn worker_loop(
    conn: Connection,
    mut queue: wayland_client::EventQueue<State>,
    mut state: State,
    wake: Arc<jfn_wake_event::WakeEvent>,
    stop: Arc<AtomicBool>,
    cursor_type: Arc<AtomicU32>,
    set_cursor_inbox: Arc<AtomicBool>,
) {
    let display_fd = conn.as_fd().as_raw_fd();
    let wake_fd = wake.fd();
    let repeat_fd = state.repeat_timer_fd.as_raw_fd();
    let qh = queue.handle();
    loop {
        // Apply any pending cursor change before we block.
        if set_cursor_inbox.swap(false, Ordering::Acquire) {
            // cursor_type already reflects the desired value (writer updates
            // it before signalling); this just ensures we re-issue the
            // Wayland request on the current pointer/serial.
            state.apply_cursor(&qh);
            let _ = conn.flush();
        }

        let _ = queue.dispatch_pending(&mut state);
        let _ = conn.flush();

        let read_guard = match queue.prepare_read() {
            Some(g) => g,
            None => continue,
        };

        let mut pfds = [
            libc::pollfd {
                fd: display_fd,
                events: libc::POLLIN,
                revents: 0,
            },
            libc::pollfd {
                fd: wake_fd,
                events: libc::POLLIN,
                revents: 0,
            },
            libc::pollfd {
                fd: repeat_fd,
                events: libc::POLLIN,
                revents: 0,
            },
        ];
        let r = unsafe { libc::poll(pfds.as_mut_ptr(), pfds.len() as _, -1) };
        if r < 0 {
            let err = std::io::Error::last_os_error();
            drop(read_guard);
            if err.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            break;
        }

        if pfds[0].revents & libc::POLLIN != 0 {
            if read_guard.read().is_err() {
                break;
            }
        } else {
            drop(read_guard);
        }
        if pfds[0].revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
            break;
        }
        if pfds[1].revents & libc::POLLIN != 0 {
            wake.drain();
            // Wake reasons: cursor change request, or cleanup.
            if stop.load(Ordering::Relaxed) {
                let _ = queue.dispatch_pending(&mut state);
                break;
            }
            // Cursor change is handled at the top of the next iteration.
        }
        // Dispatch before the repeat fd: an unread release event would
        // otherwise leave state.repeat_key stale for this check.
        let _ = queue.dispatch_pending(&mut state);

        if pfds[2].revents & libc::POLLIN != 0 {
            // Drain the expiration count so a level-triggered re-fire
            // doesn't spin the loop, then resend the held key.
            let mut buf = [0u8; 8];
            unsafe {
                libc::read(repeat_fd, buf.as_mut_ptr().cast(), buf.len());
            }
            state.fire_key_repeat();
        }
    }

    let _ = cursor_type;
}

fn init_impl(display: *mut c_void, cb: Callbacks) -> Option<InputThread> {
    if display.is_null() {
        return None;
    }
    let wake = Arc::new(jfn_wake_event::WakeEvent::new()?);
    let repeat_timer_fd = unsafe {
        libc::timerfd_create(
            libc::CLOCK_MONOTONIC,
            libc::TFD_NONBLOCK | libc::TFD_CLOEXEC,
        )
    };
    if repeat_timer_fd < 0 {
        return None;
    }
    // Wrapped now so an early return below can't leak it.
    let repeat_timer_fd = unsafe { OwnedFd::from_raw_fd(repeat_timer_fd) };
    let backend = unsafe { Backend::from_foreign_display(display as *mut _) };
    let conn = Connection::from_backend(backend);
    let (globals, queue) = registry_queue_init::<State>(&conn).ok()?;
    let qh = queue.handle();

    let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=8, ()).ok()?;
    let cursor_mgr: Option<WpCursorShapeManagerV1> = globals.bind(&qh, 1..=1, ()).ok();

    let cursor_type = Arc::new(AtomicU32::new(CursorShape::Pointer.as_raw() as u32));
    let set_cursor_inbox = Arc::new(AtomicBool::new(false));
    *KB_FOCUS_CB.lock() = cb.kb_focus;

    let state = State {
        cb,
        seat: Some(seat),
        pointer: None,
        keyboard: None,
        cursor_mgr,
        cursor_dev: None,
        ptr_x: 0.0,
        ptr_y: 0.0,
        main_ptr_x: 0.0,
        main_ptr_y: 0.0,
        pointer_serial: 0,
        mouse_button_modifiers: 0,
        popup_swallowed_buttons: 0,
        scroll_dx: 0.0,
        scroll_dy: 0.0,
        scroll_v120_x: 0,
        scroll_v120_y: 0,
        scroll_have_v120: false,
        xkb_ctx: xkb::Context::new(xkb::CONTEXT_NO_FLAGS),
        xkb_kmap: None,
        xkb_st: None,
        modifiers: 0,
        cursor_type: cursor_type.clone(),
        menu_focus: false,
        repeat_timer_fd,
        repeat_rate: 0,
        repeat_delay: 0,
        repeat_key: None,
    };

    let stop = Arc::new(AtomicBool::new(false));
    let cursor_type_thread = cursor_type.clone();
    let inbox_thread = set_cursor_inbox.clone();
    let stop_thread = stop.clone();
    let wake_thread = wake.clone();
    let worker = thread::spawn(move || {
        worker_loop(
            conn,
            queue,
            state,
            wake_thread,
            stop_thread,
            cursor_type_thread,
            inbox_thread,
        )
    });
    Some(InputThread {
        cursor_type,
        set_cursor_inbox,
        stop,
        wake,
        worker: Mutex::new(Some(worker)),
    })
}

/// # Safety
/// `display` must be a valid `wl_display*`.
pub unsafe fn init(display: *mut c_void, callbacks: &Callbacks) -> *mut InputThread {
    match init_impl(display, *callbacks) {
        Some(c) => Box::into_raw(Box::new(c)),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `ctx` must be a pointer returned by [`init`] (or null).
pub unsafe fn set_cursor(ctx: *mut InputThread, cef_cursor_type: u32) {
    let Some(c) = (unsafe { ctx.as_ref() }) else {
        return;
    };
    c.cursor_type.store(cef_cursor_type, Ordering::Relaxed);
    c.set_cursor_inbox.store(true, Ordering::Release);
    // Wake the input thread so it picks up the cursor change.
    c.wake.signal();
}

/// # Safety
/// `ctx` must be the pointer returned by [`init`] (or
/// null). Calling twice with the same non-null `ctx` causes use-after-free.
pub unsafe fn cleanup(ctx: *mut InputThread) {
    if ctx.is_null() {
        return;
    }
    let mut boxed = unsafe { Box::from_raw(ctx) };
    *KB_FOCUS_CB.lock() = None;
    boxed.stop.store(true, Ordering::Relaxed);
    boxed.wake.signal();
    if let Some(w) = boxed.worker.get_mut().take() {
        let _ = w.join();
    }
    // The WakeEvent closes its fd when the last Arc (worker's + this one) drops.
}
