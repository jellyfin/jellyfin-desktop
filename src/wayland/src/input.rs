//! Wayland input layer.
//!
//! Wraps a foreign-owned wl_display (created by C++ platform_wayland), opens
//! its own EventQueue, binds wl_seat + wp_cursor_shape_manager_v1 on its own
//! registry view, and runs a dedicated input thread that polls the display
//! fd. Input events come back to C++ as primitives via JfnInputCallbacks so
//! no CEF-typed structs cross the FFI boundary.

use std::ffi::{c_int, c_void};
use std::os::fd::{AsFd, AsRawFd};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};

use memmap2::MmapOptions;
use wayland_backend::client::Backend;
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{
    wl_keyboard, wl_pointer, wl_registry, wl_seat, wl_surface,
};
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle, WEnum};
use wayland_protocols::wp::cursor_shape::v1::client::{
    wp_cursor_shape_device_v1::{self, WpCursorShapeDeviceV1},
    wp_cursor_shape_manager_v1::WpCursorShapeManagerV1,
};
use xkbcommon::xkb;

// CEF EVENTFLAG values (kept in sync with include/internal/cef_types.h).
const EVENTFLAG_SHIFT_DOWN: u32 = 1 << 1;
const EVENTFLAG_CONTROL_DOWN: u32 = 1 << 2;
const EVENTFLAG_ALT_DOWN: u32 = 1 << 3;
const EVENTFLAG_LEFT_MOUSE_BUTTON: u32 = 1 << 4;
const EVENTFLAG_MIDDLE_MOUSE_BUTTON: u32 = 1 << 5;
const EVENTFLAG_RIGHT_MOUSE_BUTTON: u32 = 1 << 6;

// CEF cef_cursor_type_t values (order from cef_types.h).
#[allow(dead_code)]
const CT_POINTER: u32 = 0;
const CT_CROSS: u32 = 1;
const CT_HAND: u32 = 2;
const CT_IBEAM: u32 = 3;
const CT_WAIT: u32 = 4;
const CT_HELP: u32 = 5;
const CT_EASTRESIZE: u32 = 6;
const CT_NORTHRESIZE: u32 = 7;
const CT_NORTHEASTRESIZE: u32 = 8;
const CT_NORTHWESTRESIZE: u32 = 9;
const CT_SOUTHRESIZE: u32 = 10;
const CT_SOUTHEASTRESIZE: u32 = 11;
const CT_SOUTHWESTRESIZE: u32 = 12;
const CT_WESTRESIZE: u32 = 13;
const CT_NORTHSOUTHRESIZE: u32 = 14;
const CT_EASTWESTRESIZE: u32 = 15;
const CT_NORTHEASTSOUTHWESTRESIZE: u32 = 16;
const CT_NORTHWESTSOUTHEASTRESIZE: u32 = 17;
const CT_COLUMNRESIZE: u32 = 18;
const CT_ROWRESIZE: u32 = 19;
const CT_MIDDLEPANNING: u32 = 20;
#[allow(dead_code)] const CT_EASTPANNING: u32 = 21;
#[allow(dead_code)] const CT_NORTHPANNING: u32 = 22;
#[allow(dead_code)] const CT_NORTHEASTPANNING: u32 = 23;
#[allow(dead_code)] const CT_NORTHWESTPANNING: u32 = 24;
#[allow(dead_code)] const CT_SOUTHPANNING: u32 = 25;
#[allow(dead_code)] const CT_SOUTHEASTPANNING: u32 = 26;
#[allow(dead_code)] const CT_SOUTHWESTPANNING: u32 = 27;
#[allow(dead_code)] const CT_WESTPANNING: u32 = 28;
const CT_MOVE: u32 = 29;
const CT_VERTICALTEXT: u32 = 30;
const CT_CELL: u32 = 31;
const CT_CONTEXTMENU: u32 = 32;
const CT_ALIAS: u32 = 33;
const CT_PROGRESS: u32 = 34;
const CT_NODROP: u32 = 35;
const CT_COPY: u32 = 36;
const CT_NONE: u32 = 37;
const CT_NOTALLOWED: u32 = 38;
const CT_ZOOMIN: u32 = 39;
const CT_ZOOMOUT: u32 = 40;
const CT_GRAB: u32 = 41;
const CT_GRABBING: u32 = 42;
const CT_MIDDLE_PANNING_VERTICAL: u32 = 43;
const CT_MIDDLE_PANNING_HORIZONTAL: u32 = 44;

fn cef_to_wl_shape(cef: u32) -> u32 {
    use wp_cursor_shape_device_v1::Shape;
    let s = match cef {
        CT_CROSS => Shape::Crosshair,
        CT_HAND => Shape::Pointer,
        CT_IBEAM => Shape::Text,
        CT_WAIT => Shape::Wait,
        CT_HELP => Shape::Help,
        CT_EASTRESIZE => Shape::EResize,
        CT_NORTHRESIZE => Shape::NResize,
        CT_NORTHEASTRESIZE => Shape::NeResize,
        CT_NORTHWESTRESIZE => Shape::NwResize,
        CT_SOUTHRESIZE => Shape::SResize,
        CT_SOUTHEASTRESIZE => Shape::SeResize,
        CT_SOUTHWESTRESIZE => Shape::SwResize,
        CT_WESTRESIZE => Shape::WResize,
        CT_NORTHSOUTHRESIZE => Shape::NsResize,
        CT_EASTWESTRESIZE => Shape::EwResize,
        CT_NORTHEASTSOUTHWESTRESIZE => Shape::NeswResize,
        CT_NORTHWESTSOUTHEASTRESIZE => Shape::NwseResize,
        CT_COLUMNRESIZE => Shape::ColResize,
        CT_ROWRESIZE => Shape::RowResize,
        CT_MOVE => Shape::Move,
        CT_VERTICALTEXT => Shape::VerticalText,
        CT_CELL => Shape::Cell,
        CT_CONTEXTMENU => Shape::ContextMenu,
        CT_ALIAS => Shape::Alias,
        CT_PROGRESS => Shape::Progress,
        CT_NODROP => Shape::NoDrop,
        CT_COPY => Shape::Copy,
        CT_NOTALLOWED => Shape::NotAllowed,
        CT_ZOOMIN => Shape::ZoomIn,
        CT_ZOOMOUT => Shape::ZoomOut,
        CT_GRAB => Shape::Grab,
        CT_GRABBING => Shape::Grabbing,
        CT_MIDDLEPANNING | CT_MIDDLE_PANNING_VERTICAL | CT_MIDDLE_PANNING_HORIZONTAL => {
            Shape::AllScroll
        }
        _ => Shape::Default,
    };
    s as u32
}

// linux/input-event-codes.h. Defined here to avoid pulling libc::input.
const BTN_LEFT: u32 = 0x110;
const BTN_RIGHT: u32 = 0x111;
const BTN_MIDDLE: u32 = 0x112;
const BTN_SIDE: u32 = 0x113;
const BTN_EXTRA: u32 = 0x114;
const BTN_FORWARD: u32 = 0x115;
const BTN_BACK: u32 = 0x116;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Callbacks {
    pub mouse_move: Option<unsafe extern "C" fn(x: i32, y: i32, mods: u32, leave: c_int)>,
    pub mouse_button:
        Option<unsafe extern "C" fn(button: u32, pressed: c_int, x: i32, y: i32, mods: u32)>,
    pub scroll: Option<unsafe extern "C" fn(x: i32, y: i32, dx: i32, dy: i32, mods: u32)>,
    pub history_nav: Option<unsafe extern "C" fn(forward: c_int)>,
    pub kb_focus: Option<unsafe extern "C" fn(gained: c_int)>,
    pub key:
        Option<unsafe extern "C" fn(keysym: u32, native_code: u32, mods: u32, pressed: c_int)>,
    pub char_: Option<unsafe extern "C" fn(codepoint: u32, mods: u32, native_code: u32)>,
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
    pointer_serial: u32,
    mouse_button_modifiers: u32,

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
}

impl State {
    fn cef_modifiers(&self) -> u32 {
        self.modifiers | self.mouse_button_modifiers
    }

    fn refresh_modifiers(&mut self) {
        let Some(st) = &self.xkb_st else {
            self.modifiers = 0;
            return;
        };
        let mut m = 0;
        if st.mod_name_is_active(xkb::MOD_NAME_SHIFT, xkb::STATE_MODS_EFFECTIVE) {
            m |= EVENTFLAG_SHIFT_DOWN;
        }
        if st.mod_name_is_active(xkb::MOD_NAME_CTRL, xkb::STATE_MODS_EFFECTIVE) {
            m |= EVENTFLAG_CONTROL_DOWN;
        }
        if st.mod_name_is_active(xkb::MOD_NAME_ALT, xkb::STATE_MODS_EFFECTIVE) {
            m |= EVENTFLAG_ALT_DOWN;
        }
        self.modifiers = m;
    }

    fn apply_cursor(&mut self, qh: &QueueHandle<Self>) {
        let cef = self.cursor_type.load(Ordering::Relaxed);
        let Some(pointer) = &self.pointer else { return };
        if self.pointer_serial == 0 {
            return;
        }
        if cef == CT_NONE {
            pointer.set_cursor(self.pointer_serial, None, 0, 0);
            return;
        }
        if self.cursor_dev.is_none() {
            if let Some(mgr) = &self.cursor_mgr {
                self.cursor_dev = Some(mgr.get_pointer(pointer, qh, ()));
            }
        }
        if let Some(dev) = &self.cursor_dev {
            let shape: wp_cursor_shape_device_v1::Shape = unsafe {
                std::mem::transmute::<u32, wp_cursor_shape_device_v1::Shape>(cef_to_wl_shape(cef))
            };
            dev.set_shape(self.pointer_serial, shape);
        }
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
            Event::Enter { serial, surface_x, surface_y, .. } => {
                state.pointer_serial = serial;
                state.apply_cursor(qh);
                state.ptr_x = surface_x;
                state.ptr_y = surface_y;
                if let Some(f) = state.cb.mouse_move {
                    unsafe {
                        f(state.ptr_x as i32, state.ptr_y as i32, state.cef_modifiers(), 0);
                    }
                }
            }
            Event::Leave { .. } => {
                if let Some(f) = state.cb.mouse_move {
                    unsafe {
                        f(state.ptr_x as i32, state.ptr_y as i32, state.cef_modifiers(), 1);
                    }
                }
            }
            Event::Motion { surface_x, surface_y, .. } => {
                state.ptr_x = surface_x;
                state.ptr_y = surface_y;
                if let Some(f) = state.cb.mouse_move {
                    unsafe {
                        f(state.ptr_x as i32, state.ptr_y as i32, state.cef_modifiers(), 0);
                    }
                }
            }
            Event::Button { button, state: bs, .. } => {
                let pressed =
                    matches!(bs, WEnum::Value(wl_pointer::ButtonState::Pressed));
                if button == BTN_SIDE
                    || button == BTN_EXTRA
                    || button == BTN_BACK
                    || button == BTN_FORWARD
                {
                    if pressed {
                        let forward = button == BTN_EXTRA || button == BTN_FORWARD;
                        if let Some(f) = state.cb.history_nav {
                            unsafe { f(if forward { 1 } else { 0 }) };
                        }
                    }
                    return;
                }
                let flag = match button {
                    BTN_LEFT => EVENTFLAG_LEFT_MOUSE_BUTTON,
                    BTN_RIGHT => EVENTFLAG_RIGHT_MOUSE_BUTTON,
                    BTN_MIDDLE => EVENTFLAG_MIDDLE_MOUSE_BUTTON,
                    _ => return,
                };
                if pressed {
                    state.mouse_button_modifiers |= flag;
                } else {
                    state.mouse_button_modifiers &= !flag;
                }
                if let Some(f) = state.cb.mouse_button {
                    unsafe {
                        f(
                            button,
                            if pressed { 1 } else { 0 },
                            state.ptr_x as i32,
                            state.ptr_y as i32,
                            state.cef_modifiers(),
                        );
                    }
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
                if let Some(f) = state.cb.scroll {
                    unsafe {
                        f(
                            state.ptr_x as i32,
                            state.ptr_y as i32,
                            dx,
                            dy,
                            state.cef_modifiers(),
                        );
                    }
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
            Event::Enter { .. } => {
                if let Some(f) = state.cb.kb_focus {
                    unsafe { f(1) };
                }
            }
            Event::Leave { .. } => {
                if let Some(f) = state.cb.kb_focus {
                    unsafe { f(0) };
                }
            }
            Event::Key { key, state: ks, .. } => {
                let Some(st) = &state.xkb_st else { return };
                let kc: xkb::Keycode = (key + 8).into();
                let sym = st.key_get_one_sym(kc);
                let pressed =
                    matches!(ks, WEnum::Value(wl_keyboard::KeyState::Pressed));
                if let Some(f) = state.cb.key {
                    unsafe {
                        f(sym.into(), key, state.modifiers, if pressed { 1 } else { 0 });
                    }
                }
                if pressed {
                    let cp = st.key_get_utf32(kc);
                    if cp > 0 {
                        if let Some(f) = state.cb.char_ {
                            unsafe { f(cp, state.modifiers, key) };
                        }
                    }
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

pub struct JfnInputWayland {
    cursor_type: Arc<AtomicU32>,
    set_cursor_inbox: Arc<Mutex<Option<u32>>>,
    stop: Arc<AtomicBool>,
    wake_fd: c_int,
    worker: Mutex<Option<JoinHandle<()>>>,
}

fn worker_loop(
    conn: Connection,
    mut queue: wayland_client::EventQueue<State>,
    mut state: State,
    wake_fd: c_int,
    stop: Arc<AtomicBool>,
    cursor_type: Arc<AtomicU32>,
    set_cursor_inbox: Arc<Mutex<Option<u32>>>,
) {
    let display_fd = conn.as_fd().as_raw_fd();
    let qh = queue.handle();
    loop {
        // Apply any pending cursor change before we block.
        let pending = set_cursor_inbox.lock().unwrap().take();
        if pending.is_some() {
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
            libc::pollfd { fd: display_fd, events: libc::POLLIN, revents: 0 },
            libc::pollfd { fd: wake_fd, events: libc::POLLIN, revents: 0 },
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
            // Drain wake fd.
            let mut buf = [0u8; 64];
            loop {
                let n = unsafe {
                    libc::read(wake_fd, buf.as_mut_ptr() as *mut c_void, buf.len())
                };
                if n <= 0 {
                    break;
                }
            }
            // Wake reasons: cursor change request, or cleanup.
            if stop.load(Ordering::Relaxed) {
                let _ = queue.dispatch_pending(&mut state);
                break;
            }
            // Cursor change is handled at the top of the next iteration.
        }

        let _ = queue.dispatch_pending(&mut state);
    }

    let _ = cursor_type;
}

fn init_impl(display: *mut c_void, cb: Callbacks) -> Option<JfnInputWayland> {
    if display.is_null() {
        return None;
    }
    let wake_fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    if wake_fd < 0 {
        return None;
    }
    let backend = unsafe { Backend::from_foreign_display(display as *mut _) };
    let conn = Connection::from_backend(backend);
    let (globals, queue) = registry_queue_init::<State>(&conn).ok()?;
    let qh = queue.handle();

    let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=8, ()).ok()?;
    let cursor_mgr: Option<WpCursorShapeManagerV1> = globals.bind(&qh, 1..=1, ()).ok();

    let cursor_type = Arc::new(AtomicU32::new(CT_POINTER));
    let set_cursor_inbox = Arc::new(Mutex::new(None));

    let state = State {
        cb,
        seat: Some(seat),
        pointer: None,
        keyboard: None,
        cursor_mgr,
        cursor_dev: None,
        ptr_x: 0.0,
        ptr_y: 0.0,
        pointer_serial: 0,
        mouse_button_modifiers: 0,
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
    };

    let stop = Arc::new(AtomicBool::new(false));
    let cursor_type_thread = cursor_type.clone();
    let inbox_thread = set_cursor_inbox.clone();
    let stop_thread = stop.clone();
    let worker = thread::spawn(move || {
        worker_loop(conn, queue, state, wake_fd, stop_thread, cursor_type_thread, inbox_thread)
    });
    Some(JfnInputWayland {
        cursor_type,
        set_cursor_inbox,
        stop,
        wake_fd,
        worker: Mutex::new(Some(worker)),
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_input_wayland_init(
    display: *mut c_void,
    callbacks: *const Callbacks,
) -> *mut JfnInputWayland {
    let Some(cb) = (unsafe { callbacks.as_ref() }) else {
        return std::ptr::null_mut();
    };
    let cb = *cb;
    match init_impl(display, cb) {
        Some(c) => Box::into_raw(Box::new(c)),
        None => std::ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_input_wayland_start(_ctx: *mut JfnInputWayland) {
    // Thread starts in init; this is kept for ABI compatibility with the
    // C++ API which had an explicit start step.
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_input_wayland_set_cursor(
    ctx: *mut JfnInputWayland,
    cef_cursor_type: u32,
) {
    let Some(c) = (unsafe { ctx.as_ref() }) else { return };
    c.cursor_type.store(cef_cursor_type, Ordering::Relaxed);
    *c.set_cursor_inbox.lock().unwrap() = Some(cef_cursor_type);
    // Wake the input thread so it picks up the cursor change.
    let v: u64 = 1;
    unsafe {
        libc::write(c.wake_fd, &v as *const u64 as *const c_void, 8);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_input_wayland_cleanup(ctx: *mut JfnInputWayland) {
    if ctx.is_null() {
        return;
    }
    let mut boxed = unsafe { Box::from_raw(ctx) };
    boxed.stop.store(true, Ordering::Relaxed);
    let v: u64 = 1;
    unsafe {
        libc::write(boxed.wake_fd, &v as *const u64 as *const c_void, 8);
    }
    if let Some(w) = boxed.worker.get_mut().unwrap().take() {
        let _ = w.join();
    }
    unsafe { libc::close(boxed.wake_fd) };
}
