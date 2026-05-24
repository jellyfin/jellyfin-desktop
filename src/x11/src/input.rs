//! X11 input thread.
//!
//! Owns its own `Arc<xcb::Connection>` and pumps the event queue via
//! `poll()` over (xcb fd, shutdown wake fd, cursor wake fd). xkb state
//! lives on this thread only; cursor changes from other threads are
//! queued onto a `Mutex` and signalled via an eventfd.

use std::ffi::{CString, c_int};
use std::os::fd::AsRawFd;
use std::os::raw::c_uchar;
use std::sync::{Arc, Mutex};

use xcb::{Xid, XidNew, x};
use xcb_util_cursor_sys as cursor_ffi;
use xkbcommon::xkb::{self, x11 as xkb_x11};

use crate::x11_state::MUT;

use jfn_input::{
    jfn_input_dispatch_char, jfn_input_dispatch_history_nav, jfn_input_dispatch_key_raw,
    jfn_input_dispatch_mouse_button, jfn_input_dispatch_mouse_move, jfn_input_dispatch_scroll,
};
use jfn_playback::ingest_driver::jfn_playback_display_scale;
use jfn_playback::shutdown::{jfn_shutdown_event, jfn_shutdown_initiate};
use jfn_playback::wake_event::{
    jfn_wake_event_drain, jfn_wake_event_fd, jfn_wake_event_free, jfn_wake_event_new,
    jfn_wake_event_signal,
};

// CEF event flag bits (cef_event_flags_t).
const EVENTFLAG_SHIFT_DOWN: u32 = 1 << 1;
const EVENTFLAG_CONTROL_DOWN: u32 = 1 << 2;
const EVENTFLAG_ALT_DOWN: u32 = 1 << 3;
const EVENTFLAG_LEFT_MOUSE_BUTTON: u32 = 1 << 4;
const EVENTFLAG_MIDDLE_MOUSE_BUTTON: u32 = 1 << 5;
const EVENTFLAG_RIGHT_MOUSE_BUTTON: u32 = 1 << 6;

// CEF cursor type values that need special handling (from cef_types.h).
const CT_NONE: u32 = 37;

const XKB_KEY_XF86BACK: u32 = 0x1008ff26;
const XKB_KEY_XF86FORWARD: u32 = 0x1008ff27;

/// Cursor request queued for the input thread.
enum CursorReq {
    Set(u32),
}

pub struct CursorMailbox {
    queue: Mutex<Vec<CursorReq>>,
    wake: *mut jfn_playback::WakeEvent,
}

unsafe impl Send for CursorMailbox {}
unsafe impl Sync for CursorMailbox {}

impl CursorMailbox {
    fn new() -> Self {
        let wake = unsafe { jfn_wake_event_new() };
        Self {
            queue: Mutex::new(Vec::new()),
            wake,
        }
    }
    fn push(&self, req: CursorReq) {
        self.queue.lock().unwrap().push(req);
        unsafe { jfn_wake_event_signal(self.wake) };
    }
    fn drain(&self) -> Vec<CursorReq> {
        let mut q = self.queue.lock().unwrap();
        std::mem::take(&mut *q)
    }
}

impl Drop for CursorMailbox {
    fn drop(&mut self) {
        if !self.wake.is_null() {
            unsafe { jfn_wake_event_free(self.wake) };
        }
    }
}

pub struct Handle {
    pub conn: Arc<xcb::Connection>,
    pub parent: x::Window,
    join: Option<std::thread::JoinHandle<()>>,
    pub mailbox: Arc<CursorMailbox>,
}

impl Handle {
    pub fn join(&mut self) {
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

struct State {
    conn: Arc<xcb::Connection>,
    window: x::Window,
    screen_num: i32,

    xkb_ctx: xkb::Context,
    xkb_kmap: Option<xkb::Keymap>,
    xkb_st: Option<xkb::State>,
    xkb_device_id: i32,
    xkb_base_event: u8,
    modifiers: u32,

    ptr_x: i32,
    ptr_y: i32,
    mouse_button_modifiers: u32,

    cursor_type: u32,
    current_cursor: u32, // xcb_cursor_t id, 0 == none
    cursor_ctx: *mut cursor_ffi::xcb_cursor_context_t,

    mailbox: Arc<CursorMailbox>,
}

unsafe impl Send for State {}

fn xkb_to_cef_mods(st: &xkb::State) -> u32 {
    let mut m = 0u32;
    if st.mod_name_is_active(xkb::MOD_NAME_SHIFT, xkb::STATE_MODS_EFFECTIVE) {
        m |= EVENTFLAG_SHIFT_DOWN;
    }
    if st.mod_name_is_active(xkb::MOD_NAME_CTRL, xkb::STATE_MODS_EFFECTIVE) {
        m |= EVENTFLAG_CONTROL_DOWN;
    }
    if st.mod_name_is_active(xkb::MOD_NAME_ALT, xkb::STATE_MODS_EFFECTIVE) {
        m |= EVENTFLAG_ALT_DOWN;
    }
    m
}

fn build_cursor_screen(screen: &x::Screen, allowed_depths_len: u8) -> cursor_ffi::xcb_screen_t {
    cursor_ffi::xcb_screen_t {
        root: screen.root().resource_id(),
        default_colormap: screen.default_colormap().resource_id(),
        white_pixel: screen.white_pixel(),
        black_pixel: screen.black_pixel(),
        current_input_masks: screen.current_input_masks().bits(),
        width_in_pixels: screen.width_in_pixels(),
        height_in_pixels: screen.height_in_pixels(),
        width_in_millimeters: screen.width_in_millimeters(),
        height_in_millimeters: screen.height_in_millimeters(),
        min_installed_maps: screen.min_installed_maps(),
        max_installed_maps: screen.max_installed_maps(),
        root_visual: screen.root_visual(),
        backing_stores: screen.backing_stores() as u8,
        save_unders: screen.save_unders() as c_uchar,
        root_depth: screen.root_depth(),
        allowed_depths_len,
    }
}

fn cef_cursor_to_name(t: u32) -> &'static str {
    // Values follow cef_cursor_type_t in cef_types.h.
    match t {
        1 => "crosshair",        // CT_CROSS
        2 => "pointer",          // CT_HAND
        3 => "text",             // CT_IBEAM
        4 => "wait",             // CT_WAIT
        5 => "help",             // CT_HELP
        6 => "e-resize",         // CT_EASTRESIZE
        7 => "n-resize",         // CT_NORTHRESIZE
        8 => "ne-resize",        // CT_NORTHEASTRESIZE
        9 => "nw-resize",        // CT_NORTHWESTRESIZE
        10 => "s-resize",        // CT_SOUTHRESIZE
        11 => "se-resize",       // CT_SOUTHEASTRESIZE
        12 => "sw-resize",       // CT_SOUTHWESTRESIZE
        13 => "w-resize",        // CT_WESTRESIZE
        14 => "ns-resize",       // CT_NORTHSOUTHRESIZE
        15 => "ew-resize",       // CT_EASTWESTRESIZE
        16 => "nesw-resize",     // CT_NORTHEASTSOUTHWESTRESIZE
        17 => "nwse-resize",     // CT_NORTHWESTSOUTHEASTRESIZE
        18 => "col-resize",      // CT_COLUMNRESIZE
        19 => "row-resize",      // CT_ROWRESIZE
        20 => "all-scroll",      // CT_MIDDLEPANNING
        29 => "move",            // CT_MOVE
        30 => "vertical-text",   // CT_VERTICALTEXT
        31 => "cell",            // CT_CELL
        32 => "context-menu",    // CT_CONTEXTMENU
        33 => "alias",           // CT_ALIAS
        34 => "progress",        // CT_PROGRESS
        35 => "no-drop",         // CT_NODROP
        38 => "not-allowed",     // CT_NOTALLOWED
        39 => "zoom-in",         // CT_ZOOMIN
        40 => "zoom-out",        // CT_ZOOMOUT
        41 => "grab",            // CT_GRAB
        42 => "grabbing",        // CT_GRABBING
        43 | 44 => "all-scroll", // CT_MIDDLE_PANNING_VERTICAL/HORIZONTAL
        _ => "default",
    }
}

fn setup_xkb(conn: &xcb::Connection, st: &mut State) -> bool {
    let mut major = 0u16;
    let mut minor = 0u16;
    let mut base_event = 0u8;
    let mut base_error = 0u8;
    if !xkb_x11::setup_xkb_extension(
        conn,
        xkb_x11::MIN_MAJOR_XKB_VERSION,
        xkb_x11::MIN_MINOR_XKB_VERSION,
        xkb_x11::SetupXkbExtensionFlags::NoFlags,
        &mut major,
        &mut minor,
        &mut base_event,
        &mut base_error,
    ) {
        return false;
    }
    st.xkb_base_event = base_event;

    let device_id = xkb_x11::get_core_keyboard_device_id(conn);
    if device_id < 0 {
        return false;
    }
    st.xkb_device_id = device_id;

    let kmap =
        xkb_x11::keymap_new_from_device(&st.xkb_ctx, conn, device_id, xkb::KEYMAP_COMPILE_NO_FLAGS);
    if kmap.get_raw_ptr().is_null() {
        return false;
    }
    let state = xkb_x11::state_new_from_device(&kmap, conn, device_id);
    if state.get_raw_ptr().is_null() {
        return false;
    }
    st.xkb_kmap = Some(kmap);
    st.xkb_st = Some(state);

    let required_map = xcb::xkb::MapPart::KEY_TYPES
        | xcb::xkb::MapPart::KEY_SYMS
        | xcb::xkb::MapPart::MODIFIER_MAP
        | xcb::xkb::MapPart::EXPLICIT_COMPONENTS
        | xcb::xkb::MapPart::KEY_ACTIONS
        | xcb::xkb::MapPart::VIRTUAL_MODS
        | xcb::xkb::MapPart::VIRTUAL_MOD_MAP;
    let required_events = xcb::xkb::EventType::STATE_NOTIFY
        | xcb::xkb::EventType::MAP_NOTIFY
        | xcb::xkb::EventType::NEW_KEYBOARD_NOTIFY;

    conn.send_request(&xcb::xkb::SelectEvents {
        device_spec: device_id as xcb::xkb::DeviceSpec,
        affect_which: required_events,
        clear: xcb::xkb::EventType::empty(),
        select_all: required_events,
        affect_map: required_map,
        map: required_map,
        details: &[],
    });
    true
}

fn update_keymap(conn: &xcb::Connection, st: &mut State) {
    let kmap = xkb_x11::keymap_new_from_device(
        &st.xkb_ctx,
        conn,
        st.xkb_device_id,
        xkb::KEYMAP_COMPILE_NO_FLAGS,
    );
    if kmap.get_raw_ptr().is_null() {
        return;
    }
    let new_state = xkb_x11::state_new_from_device(&kmap, conn, st.xkb_device_id);
    if new_state.get_raw_ptr().is_null() {
        return;
    }
    st.xkb_kmap = Some(kmap);
    st.xkb_st = Some(new_state);
}

fn cef_modifiers(st: &State) -> u32 {
    st.modifiers | st.mouse_button_modifiers
}

fn to_logical(physical: i32) -> i32 {
    let scale = unsafe { jfn_playback_display_scale() };
    let s = if scale > 0.0 { scale } else { 1.0 };
    (physical as f64 / s) as i32
}

fn handle_key(st: &mut State, detail: u8, pressed: bool) {
    let Some(xst) = st.xkb_st.as_mut() else {
        return;
    };
    let kc_raw = detail as u32;
    let kc = xkb::Keycode::new(kc_raw);
    let sym: u32 = xst.key_get_one_sym(kc).raw();

    if sym == XKB_KEY_XF86BACK || sym == XKB_KEY_XF86FORWARD {
        if pressed {
            unsafe {
                jfn_input_dispatch_history_nav((sym == XKB_KEY_XF86FORWARD) as c_int);
            }
        }
        xst.update_key(
            kc,
            if pressed {
                xkb::KeyDirection::Down
            } else {
                xkb::KeyDirection::Up
            },
        );
        return;
    }

    let native = (kc_raw as i32) - 8; // X keycode → linux input code
    unsafe {
        jfn_input_dispatch_key_raw(sym, native as u32, st.modifiers, pressed as c_int);
    }

    if pressed {
        let cp = xst.key_get_utf32(kc);
        if cp > 0 {
            unsafe { jfn_input_dispatch_char(cp, st.modifiers, native as u32) };
        }
    }

    xst.update_key(
        kc,
        if pressed {
            xkb::KeyDirection::Down
        } else {
            xkb::KeyDirection::Up
        },
    );
    st.modifiers = xkb_to_cef_mods(xst);
}

fn handle_button(st: &mut State, detail: u8, event_x: i16, event_y: i16, pressed: bool) {
    let button = detail as u32;
    let x = to_logical(event_x as i32);
    let y = to_logical(event_y as i32);

    if (4..=7).contains(&button) {
        if !pressed {
            return;
        }
        let (dx, dy) = match button {
            4 => (0, 120),
            5 => (0, -120),
            6 => (120, 0),
            7 => (-120, 0),
            _ => (0, 0),
        };
        unsafe { jfn_input_dispatch_scroll(x, y, dx, dy, cef_modifiers(st)) };
        return;
    }

    if button == 8 || button == 9 {
        if pressed {
            unsafe { jfn_input_dispatch_history_nav((button == 9) as c_int) };
        }
        return;
    }

    let flag = match button {
        1 => EVENTFLAG_LEFT_MOUSE_BUTTON,
        2 => EVENTFLAG_MIDDLE_MOUSE_BUTTON,
        3 => EVENTFLAG_RIGHT_MOUSE_BUTTON,
        _ => return,
    };
    if pressed {
        st.mouse_button_modifiers |= flag;
    } else {
        st.mouse_button_modifiers &= !flag;
    }

    // dispatch.cpp expects linux/input-event-codes.h button codes.
    let code: u32 = match button {
        1 => 0x110, // BTN_LEFT
        2 => 0x112, // BTN_MIDDLE
        3 => 0x111, // BTN_RIGHT
        _ => return,
    };
    unsafe {
        jfn_input_dispatch_mouse_button(code, pressed as c_int, x, y, cef_modifiers(st));
    }
}

fn handle_motion(st: &mut State, ev: &xcb::x::MotionNotifyEvent) {
    st.ptr_x = to_logical(ev.event_x() as i32);
    st.ptr_y = to_logical(ev.event_y() as i32);
    unsafe {
        jfn_input_dispatch_mouse_move(st.ptr_x, st.ptr_y, cef_modifiers(st), 0);
    }
}

fn handle_enter(st: &mut State, ev: &xcb::x::EnterNotifyEvent) {
    st.ptr_x = to_logical(ev.event_x() as i32);
    st.ptr_y = to_logical(ev.event_y() as i32);
    apply_cursor(st, st.cursor_type);
    unsafe {
        jfn_input_dispatch_mouse_move(st.ptr_x, st.ptr_y, cef_modifiers(st), 0);
    }
}

fn handle_leave(st: &State, _ev: &xcb::x::LeaveNotifyEvent) {
    unsafe {
        jfn_input_dispatch_mouse_move(st.ptr_x, st.ptr_y, cef_modifiers(st), 1);
    }
}

fn handle_xkb_state_notify(st: &mut State, ev: &xcb::xkb::StateNotifyEvent) {
    if let Some(xst) = st.xkb_st.as_mut() {
        xst.update_mask(
            ev.base_mods().bits(),
            ev.latched_mods().bits(),
            ev.locked_mods().bits(),
            ev.base_group() as u32,
            ev.latched_group() as u32,
            ev.locked_group() as u32,
        );
        st.modifiers = xkb_to_cef_mods(xst);
    }
}

fn apply_cursor(st: &mut State, t: u32) {
    st.cursor_type = t;
    let conn = &st.conn;

    if t == CT_NONE {
        let pix: x::Pixmap = conn.generate_id();
        conn.send_request(&x::CreatePixmap {
            depth: 1,
            pid: pix,
            drawable: x::Drawable::Window(st.window),
            width: 1,
            height: 1,
        });
        let blank: x::Cursor = conn.generate_id();
        conn.send_request(&x::CreateCursor {
            cid: blank,
            source: pix,
            mask: pix,
            fore_red: 0,
            fore_green: 0,
            fore_blue: 0,
            back_red: 0,
            back_green: 0,
            back_blue: 0,
            x: 0,
            y: 0,
        });
        conn.send_request(&x::ChangeWindowAttributes {
            window: st.window,
            value_list: &[x::Cw::Cursor(blank)],
        });
        let _ = conn.flush();
        if st.current_cursor != 0 {
            let old = x::Cursor::new(st.current_cursor);
            conn.send_request(&x::FreeCursor { cursor: old });
        }
        st.current_cursor = blank.resource_id();
        conn.send_request(&x::FreePixmap { pixmap: pix });
        return;
    }

    if st.cursor_ctx.is_null() {
        return;
    }

    let name = cef_cursor_to_name(t);
    let cname = CString::new(name).unwrap();
    let cursor_id = unsafe { cursor_ffi::xcb_cursor_load_cursor(st.cursor_ctx, cname.as_ptr()) };
    if cursor_id == 0 {
        return;
    }
    let cur = x::Cursor::new(cursor_id);
    conn.send_request(&x::ChangeWindowAttributes {
        window: st.window,
        value_list: &[x::Cw::Cursor(cur)],
    });
    let _ = conn.flush();
    if st.current_cursor != 0 && st.current_cursor != cursor_id {
        let old = x::Cursor::new(st.current_cursor);
        conn.send_request(&x::FreeCursor { cursor: old });
    }
    st.current_cursor = cursor_id;
}

fn drain_cursor_requests(st: &mut State) {
    let reqs = st.mailbox.drain();
    for r in reqs {
        match r {
            CursorReq::Set(t) => apply_cursor(st, t),
        }
    }
}

fn input_thread_body(mut st: State) {
    // Resolve cursor context now that we're on the input thread (xcb-cursor
    // doesn't require any specific thread, but we keep the raw ctx pointer
    // bound to the lifetime of this thread).
    {
        let conn = st.conn.clone();
        let setup = conn.get_setup();
        if let Some(screen) = setup.roots().nth(st.screen_num as usize) {
            let allowed = screen.allowed_depths().count() as u8;
            let mut sc = build_cursor_screen(screen, allowed);
            let mut ctx_ptr: *mut cursor_ffi::xcb_cursor_context_t = std::ptr::null_mut();
            let rc = unsafe {
                cursor_ffi::xcb_cursor_context_new(
                    conn.get_raw_conn() as *mut _,
                    &mut sc as *mut _,
                    &mut ctx_ptr as *mut _,
                )
            };
            if rc == 0 {
                st.cursor_ctx = ctx_ptr;
            } else {
                eprintln!("[x11] xcb_cursor_context_new failed rc={}", rc);
            }
        }
    }

    if !setup_xkb(&st.conn.clone(), &mut st) {
        eprintln!("[x11] xkb setup failed; key input disabled");
    }

    // Subscribe to input + structure events on the window.
    let mask = x::EventMask::KEY_PRESS
        | x::EventMask::KEY_RELEASE
        | x::EventMask::BUTTON_PRESS
        | x::EventMask::BUTTON_RELEASE
        | x::EventMask::POINTER_MOTION
        | x::EventMask::ENTER_WINDOW
        | x::EventMask::LEAVE_WINDOW
        | x::EventMask::STRUCTURE_NOTIFY;
    st.conn.send_request(&x::ChangeWindowAttributes {
        window: st.window,
        value_list: &[x::Cw::EventMask(mask)],
    });
    let _ = st.conn.flush();

    let xcb_fd = st.conn.as_raw_fd();
    let shutdown_ev = unsafe { jfn_shutdown_event() };
    let shutdown_fd = unsafe { jfn_wake_event_fd(shutdown_ev) };
    let cursor_fd = unsafe { jfn_wake_event_fd(st.mailbox.wake) };

    let mut fds: [libc::pollfd; 3] = [
        libc::pollfd {
            fd: xcb_fd,
            events: libc::POLLIN,
            revents: 0,
        },
        libc::pollfd {
            fd: shutdown_fd,
            events: libc::POLLIN,
            revents: 0,
        },
        libc::pollfd {
            fd: cursor_fd,
            events: libc::POLLIN,
            revents: 0,
        },
    ];

    loop {
        let _ = st.conn.flush();
        let rc = unsafe { libc::poll(fds.as_mut_ptr(), 3, -1) };
        if rc < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            break;
        }

        if fds[1].revents & libc::POLLIN != 0 {
            // Shutdown — hide overlays from this thread before exit.
            if let Some(conn) = crate::x11_state::conn() {
                let g = MUT.lock().unwrap();
                if let Some(m) = g.as_ref() {
                    crate::lifecycle::hide_all_live_locked(&conn, m);
                }
            }
            break;
        }
        if fds[0].revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
            if let Some(conn) = crate::x11_state::conn() {
                let g = MUT.lock().unwrap();
                if let Some(m) = g.as_ref() {
                    crate::lifecycle::hide_all_live_locked(&conn, m);
                }
            }
            break;
        }
        if fds[2].revents & libc::POLLIN != 0 {
            unsafe { jfn_wake_event_drain(st.mailbox.wake) };
            drain_cursor_requests(&mut st);
        }

        while let Ok(Some(ev)) = st.conn.poll_for_event() {
            handle_event(&mut st, ev);
        }
    }

    // Cursor context is released as the thread state drops.
    if !st.cursor_ctx.is_null() {
        unsafe { cursor_ffi::xcb_cursor_context_free(st.cursor_ctx) };
        st.cursor_ctx = std::ptr::null_mut();
    }
    if st.current_cursor != 0 {
        let cur = x::Cursor::new(st.current_cursor);
        st.conn.send_request(&x::FreeCursor { cursor: cur });
        let _ = st.conn.flush();
    }
}

fn handle_event(st: &mut State, ev: xcb::Event) {
    use xcb::Event;
    match ev {
        Event::X(x::Event::KeyPress(e)) => handle_key(st, e.detail(), true),
        Event::X(x::Event::KeyRelease(e)) => handle_key(st, e.detail(), false),
        Event::X(x::Event::ButtonPress(e)) => {
            handle_button(st, e.detail(), e.event_x(), e.event_y(), true)
        }
        Event::X(x::Event::ButtonRelease(e)) => {
            handle_button(st, e.detail(), e.event_x(), e.event_y(), false)
        }
        Event::X(x::Event::MotionNotify(e)) => handle_motion(st, &e),
        Event::X(x::Event::EnterNotify(e)) => handle_enter(st, &e),
        Event::X(x::Event::LeaveNotify(e)) => handle_leave(st, &e),
        Event::X(x::Event::ConfigureNotify(_)) => {
            if let Some(conn) = crate::x11_state::conn() {
                let mut g = MUT.lock().unwrap();
                if let Some(m) = g.as_mut() {
                    crate::lifecycle::sync_overlay_positions_locked(&conn, m);
                }
            }
        }
        Event::X(x::Event::DestroyNotify(_)) => unsafe { jfn_shutdown_initiate() },
        Event::X(x::Event::ClientMessage(_)) => unsafe { jfn_shutdown_initiate() },
        Event::Xkb(xkb_ev) => {
            use xcb::xkb;
            match xkb_ev {
                xkb::Event::StateNotify(e) => handle_xkb_state_notify(st, &e),
                xkb::Event::MapNotify(_) | xkb::Event::NewKeyboardNotify(_) => {
                    let conn = st.conn.clone();
                    update_keymap(&conn, st);
                }
                _ => {}
            }
        }
        _ => {}
    }
}

pub fn start(conn: Arc<xcb::Connection>, screen_num: i32, parent: x::Window) -> Handle {
    let mailbox = Arc::new(CursorMailbox::new());
    let st = State {
        conn: conn.clone(),
        window: parent,
        screen_num,
        xkb_ctx: xkb::Context::new(xkb::CONTEXT_NO_FLAGS),
        xkb_kmap: None,
        xkb_st: None,
        xkb_device_id: -1,
        xkb_base_event: 0,
        modifiers: 0,
        ptr_x: 0,
        ptr_y: 0,
        mouse_button_modifiers: 0,
        cursor_type: 0, // CT_POINTER
        current_cursor: 0,
        cursor_ctx: std::ptr::null_mut(),
        mailbox: mailbox.clone(),
    };

    let join = std::thread::Builder::new()
        .name("jfn-x11-input".into())
        .spawn(move || input_thread_body(st))
        .expect("spawn x11 input thread");

    Handle {
        conn,
        parent,
        join: Some(join),
        mailbox,
    }
}

pub fn set_cursor(handle: &Handle, t: u32) {
    handle.mailbox.push(CursorReq::Set(t));
}
