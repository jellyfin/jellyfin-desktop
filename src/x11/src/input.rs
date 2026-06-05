//! X11 input thread.

use parking_lot::Mutex;
use std::ffi::c_int;
use std::os::fd::AsRawFd;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};

use x11rb::connection::Connection as X11rbConnection;
use x11rb::cursor::Handle as X11rbCursorHandle;
use x11rb::protocol::xproto::{
    ChangeWindowAttributesAux as X11rbChangeWindowAttributesAux,
    ConnectionExt as X11rbXprotoConnection,
};
use x11rb::resource_manager::new_from_default;
use x11rb::rust_connection::RustConnection;
use xcb::{Xid, XidNew, x};
use xkbcommon::xkb::{self, x11 as xkb_x11};

use jfn_input::{
    jfn_input_dispatch_char, jfn_input_dispatch_history_nav, jfn_input_dispatch_key_raw,
    jfn_input_dispatch_mouse_button, jfn_input_dispatch_mouse_move, jfn_input_dispatch_scroll,
};
use jfn_playback::ingest_driver::jfn_playback_display_scale;
use jfn_playback::shutdown::jfn_shutdown_register_waker;
use jfn_playback::wake_event::{
    jfn_wake_event_drain, jfn_wake_event_fd, jfn_wake_event_free, jfn_wake_event_new,
    jfn_wake_event_signal,
};

use cursor_icon::CursorIcon;
use jfn_input::buttons;
use jfn_input::xkb::to_cef_mods;
use jfn_platform_abi::cursor::CursorShape;
use jfn_platform_abi::event_flags::{
    EVENTFLAG_LEFT_MOUSE_BUTTON, EVENTFLAG_MIDDLE_MOUSE_BUTTON, EVENTFLAG_RIGHT_MOUSE_BUTTON,
};

const XKB_KEY_XF86BACK: u32 = 0x1008ff26;
const XKB_KEY_XF86FORWARD: u32 = 0x1008ff27;

#[derive(Copy, Clone)]
enum CursorReq {
    Set(CursorShape),
}

pub struct CursorMailbox {
    queue: Mutex<Vec<CursorReq>>,
    latest_type: AtomicU32,
    shutdown: std::sync::atomic::AtomicBool,
    // SAFETY: the underlying `WakeEvent` (kernel eventfd / pipe) is safe to
    // signal/drain from any thread. `Drop` frees it; freeing only happens
    // when the last `Arc<CursorMailbox>` is dropped, which by ownership
    // discipline outlives every signaller (producers hold an `Arc` clone).
    wake: *mut jfn_playback::WakeEvent,
}

unsafe impl Send for CursorMailbox {}
unsafe impl Sync for CursorMailbox {}

impl CursorMailbox {
    fn new() -> Self {
        let wake = jfn_wake_event_new();
        Self {
            queue: Mutex::new(Vec::new()),
            latest_type: AtomicU32::new(CursorShape::Pointer.as_raw() as u32),
            shutdown: std::sync::atomic::AtomicBool::new(false),
            wake,
        }
    }
    fn push(&self, req: CursorReq) {
        match req {
            CursorReq::Set(t) => self.latest_type.store(t.as_raw() as u32, Ordering::Release),
        }
        self.queue.lock().push(req);
        unsafe { jfn_wake_event_signal(self.wake) };
    }
    fn latest_type(&self) -> CursorShape {
        CursorShape::from_cef(self.latest_type.load(Ordering::Acquire) as i32)
            .unwrap_or(CursorShape::Pointer)
    }
    fn shutdown(&self) {
        self.shutdown.store(true, Ordering::Release);
        unsafe { jfn_wake_event_signal(self.wake) };
    }
    fn should_shutdown(&self) -> bool {
        self.shutdown.load(Ordering::Acquire)
    }
    fn drain(&self) -> Vec<CursorReq> {
        let mut q = self.queue.lock();
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
    join: Option<std::thread::JoinHandle<()>>,
    cursor_join: Option<std::thread::JoinHandle<()>>,
    input_join: Option<std::thread::JoinHandle<()>>,
    pub mailbox: Arc<CursorMailbox>,
    input_mailbox: Arc<InputMailbox>,
}

impl Handle {
    pub fn join(&mut self) {
        if let Some(j) = self.join.take()
            && let Err(e) = j.join()
        {
            eprintln!("[x11] input thread panicked: {e:?}");
        }
        self.mailbox.shutdown();
        if let Some(j) = self.cursor_join.take()
            && let Err(e) = j.join()
        {
            eprintln!("[x11] cursor thread panicked: {e:?}");
        }
        self.input_mailbox.shutdown();
        if let Some(j) = self.input_join.take()
            && let Err(e) = j.join()
        {
            eprintln!("[x11] input dispatch thread panicked: {e:?}");
        }
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        // Signal the shutdown waker so the input thread exits its `poll`,
        // then join. Without this, dropping a Handle mid-run leaves the
        // input thread detached and polling xcb forever.
        unsafe { jfn_wake_event_signal(x11_shutdown_waker()) };
        self.join();
    }
}

enum QueuedInputEvent {
    KeyRaw {
        sym: u32,
        native: u32,
        modifiers: u32,
        pressed: c_int,
    },
    Char {
        cp: u32,
        modifiers: u32,
        native: u32,
    },
    HistoryNav {
        forward: c_int,
    },
    MouseButton {
        code: u32,
        pressed: c_int,
        x: i32,
        y: i32,
        modifiers: u32,
    },
    MouseMove {
        x: i32,
        y: i32,
        modifiers: u32,
        leave: c_int,
    },
    Scroll {
        x: i32,
        y: i32,
        dx: i32,
        dy: i32,
        modifiers: u32,
    },
}

pub struct InputMailbox {
    queue: Mutex<Vec<QueuedInputEvent>>,
    shutdown: std::sync::atomic::AtomicBool,
    wake: *mut jfn_playback::WakeEvent,
}

unsafe impl Send for InputMailbox {}
unsafe impl Sync for InputMailbox {}

impl InputMailbox {
    fn new() -> Self {
        Self {
            queue: Mutex::new(Vec::new()),
            shutdown: std::sync::atomic::AtomicBool::new(false),
            wake: jfn_wake_event_new(),
        }
    }

    fn push(&self, ev: QueuedInputEvent) {
        self.queue.lock().push(ev);
        unsafe { jfn_wake_event_signal(self.wake) };
    }

    fn shutdown(&self) {
        self.shutdown.store(true, Ordering::Release);
        unsafe { jfn_wake_event_signal(self.wake) };
    }

    fn should_shutdown(&self) -> bool {
        self.shutdown.load(Ordering::Acquire)
    }

    fn drain(&self) -> Vec<QueuedInputEvent> {
        let mut q = self.queue.lock();
        std::mem::take(&mut *q)
    }
}

impl Drop for InputMailbox {
    fn drop(&mut self) {
        if !self.wake.is_null() {
            unsafe { jfn_wake_event_free(self.wake) };
        }
    }
}

struct State {
    conn: Arc<xcb::Connection>,
    window: u32,
    root: u32,
    net_active_window: u32,
    xkb_ctx: xkb::Context,
    xkb_kmap: Option<xkb::Keymap>,
    xkb_st: Option<xkb::State>,
    xkb_device_id: i32,
    xkb_base_event: u8,
    modifiers: u32,

    ptr_x: i32,
    ptr_y: i32,
    mouse_button_modifiers: u32,

    mailbox: Arc<CursorMailbox>,
    input_mailbox: Arc<InputMailbox>,
}

unsafe impl Send for State {}

fn cef_cursor_to_icon(shape: CursorShape) -> CursorIcon {
    use CursorShape::*;
    match shape {
        Cross => CursorIcon::Crosshair,
        Hand => CursorIcon::Pointer,
        IBeam => CursorIcon::Text,
        Wait => CursorIcon::Wait,
        Help => CursorIcon::Help,
        EastResize => CursorIcon::EResize,
        NorthResize => CursorIcon::NResize,
        NorthEastResize => CursorIcon::NeResize,
        NorthWestResize => CursorIcon::NwResize,
        SouthResize => CursorIcon::SResize,
        SouthEastResize => CursorIcon::SeResize,
        SouthWestResize => CursorIcon::SwResize,
        WestResize => CursorIcon::WResize,
        NorthSouthResize => CursorIcon::NsResize,
        EastWestResize => CursorIcon::EwResize,
        NorthEastSouthWestResize => CursorIcon::NeswResize,
        NorthWestSouthEastResize => CursorIcon::NwseResize,
        ColumnResize => CursorIcon::ColResize,
        RowResize => CursorIcon::RowResize,
        MiddlePanning | MiddlePanningVertical | MiddlePanningHorizontal => CursorIcon::AllScroll,
        Move => CursorIcon::Move,
        VerticalText => CursorIcon::VerticalText,
        Cell => CursorIcon::Cell,
        ContextMenu => CursorIcon::ContextMenu,
        Alias => CursorIcon::Alias,
        Progress => CursorIcon::Progress,
        NoDrop => CursorIcon::NoDrop,
        Copy => CursorIcon::Copy,
        NotAllowed => CursorIcon::NotAllowed,
        ZoomIn => CursorIcon::ZoomIn,
        ZoomOut => CursorIcon::ZoomOut,
        Grab => CursorIcon::Grab,
        Grabbing => CursorIcon::Grabbing,
        _ => CursorIcon::Default,
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
    let scale = jfn_playback_display_scale();
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
            st.input_mailbox.push(QueuedInputEvent::HistoryNav {
                forward: (sym == XKB_KEY_XF86FORWARD) as c_int,
            });
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
    st.input_mailbox.push(QueuedInputEvent::KeyRaw {
        sym,
        native: native as u32,
        modifiers: st.modifiers,
        pressed: pressed as c_int,
    });

    if pressed {
        let cp = xst.key_get_utf32(kc);
        if cp > 0 {
            st.input_mailbox.push(QueuedInputEvent::Char {
                cp,
                modifiers: st.modifiers,
                native: native as u32,
            });
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
    st.modifiers = to_cef_mods(xst);
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
        st.input_mailbox.push(QueuedInputEvent::Scroll {
            x,
            y,
            dx,
            dy,
            modifiers: cef_modifiers(st),
        });
        return;
    }

    if button == 8 || button == 9 {
        if pressed {
            st.input_mailbox.push(QueuedInputEvent::HistoryNav {
                forward: (button == 9) as c_int,
            });
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

    // Browser bridge expects linux/input-event-codes.h button codes.
    let code: u32 = match button {
        1 => buttons::BTN_LEFT,
        2 => buttons::BTN_MIDDLE,
        3 => buttons::BTN_RIGHT,
        _ => return,
    };
    if pressed {
        activate_parent(st);
    }
    st.input_mailbox.push(QueuedInputEvent::MouseButton {
        code,
        pressed: pressed as c_int,
        x,
        y,
        modifiers: cef_modifiers(st),
    });
}

fn activate_parent(st: &State) {
    if st.root == 0 || st.net_active_window == 0 {
        return;
    }
    let ev = x::ClientMessageEvent::new(
        x::Window::new(st.window),
        x::Atom::new(st.net_active_window),
        x::ClientMessageData::Data32([2, 0, 0, 0, 0]),
    );
    st.conn.send_request(&x::SendEvent {
        propagate: false,
        destination: x::SendEventDest::Window(x::Window::new(st.root)),
        event_mask: x::EventMask::SUBSTRUCTURE_NOTIFY | x::EventMask::SUBSTRUCTURE_REDIRECT,
        event: &ev,
    });
    let _ = st.conn.flush();
}

fn handle_motion(st: &mut State, ev: &xcb::x::MotionNotifyEvent) {
    st.ptr_x = to_logical(ev.event_x() as i32);
    st.ptr_y = to_logical(ev.event_y() as i32);
    st.input_mailbox.push(QueuedInputEvent::MouseMove {
        x: st.ptr_x,
        y: st.ptr_y,
        modifiers: cef_modifiers(st),
        leave: 0,
    });
}

fn handle_enter(st: &mut State, ev: &xcb::x::EnterNotifyEvent) {
    st.ptr_x = to_logical(ev.event_x() as i32);
    st.ptr_y = to_logical(ev.event_y() as i32);
    st.mailbox.push(CursorReq::Set(st.mailbox.latest_type()));
    st.input_mailbox.push(QueuedInputEvent::MouseMove {
        x: st.ptr_x,
        y: st.ptr_y,
        modifiers: cef_modifiers(st),
        leave: 0,
    });
}

fn handle_leave(st: &State, _ev: &xcb::x::LeaveNotifyEvent) {
    st.input_mailbox.push(QueuedInputEvent::MouseMove {
        x: st.ptr_x,
        y: st.ptr_y,
        modifiers: cef_modifiers(st),
        leave: 1,
    });
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
        st.modifiers = to_cef_mods(xst);
    }
}

struct CursorState {
    conn: Arc<RustConnection>,
    window: u32,
    // Never freed: `load_cursor` caches by name and hands back the same id, so
    // freeing leaves a dangling id the next lookup would re-hand out.
    cache: std::collections::HashMap<CursorShape, u32>,
    cursor_handle: Option<X11rbCursorHandle>,
}

unsafe impl Send for CursorState {}

fn live_overlay_windows() -> Vec<u32> {
    let g = crate::x11_state::MUT.lock();
    g.as_ref()
        .map(|m| {
            crate::lifecycle::snapshot_live_overlays_locked(m)
                .into_iter()
                .map(|s| s.window)
                .collect()
        })
        .unwrap_or_default()
}

fn apply_cursor(st: &mut CursorState, shape: CursorShape) {
    let conn = &st.conn;
    // Pointer sits over the grabbed overlay windows, so the cursor must be set on
    // them, not the mpv window beneath.
    let windows = live_overlay_windows();
    if windows.is_empty() {
        return;
    }

    let cursor_id = match st.cache.get(&shape) {
        Some(&id) => id,
        None => {
            let id = if shape == CursorShape::None {
                let Ok(pix) = conn.generate_id() else {
                    return;
                };
                let _ = conn.create_pixmap(1, pix, st.window, 1, 1);
                let Ok(blank) = conn.generate_id() else {
                    let _ = conn.free_pixmap(pix);
                    return;
                };
                let _ = conn.create_cursor(blank, pix, pix, 0, 0, 0, 0, 0, 0, 0, 0);
                let _ = conn.free_pixmap(pix);
                blank
            } else {
                let Some(cursor_handle) = st.cursor_handle.as_ref() else {
                    return;
                };
                let name = cef_cursor_to_icon(shape).name();
                let Ok(id) = cursor_handle.load_cursor(&**conn, name) else {
                    return;
                };
                if id == 0 {
                    return;
                }
                id
            };
            st.cache.insert(shape, id);
            id
        }
    };

    for w in &windows {
        let aux = X11rbChangeWindowAttributesAux::new().cursor(cursor_id);
        let _ = conn.change_window_attributes(*w, &aux);
    }
    let _ = conn.flush();
}

fn drain_cursor_requests(st: &mut CursorState, mailbox: &CursorMailbox) {
    let reqs = mailbox.drain();
    for r in reqs {
        match r {
            CursorReq::Set(t) => apply_cursor(st, t),
        }
    }
}

/// Per-process X11 shutdown waker. Allocated on first use and registered
/// with the shutdown fan-out so the input thread can `poll()` its fd
/// alongside xcb + the cursor mailbox.
pub(crate) fn x11_shutdown_waker() -> *const jfn_playback::WakeEvent {
    use std::sync::OnceLock;
    static EV: OnceLock<&'static jfn_playback::WakeEvent> = OnceLock::new();
    *EV.get_or_init(|| {
        let raw = jfn_playback::WakeEvent::new().expect("x11 shutdown waker allocation");
        let leaked: &'static jfn_playback::WakeEvent = Box::leak(Box::new(raw));
        jfn_shutdown_register_waker(leaked);
        leaked
    }) as *const _
}

fn input_thread_body(mut st: State) {
    if !setup_xkb(&st.conn.clone(), &mut st) {
        eprintln!("[x11] xkb setup failed; key input disabled");
    }

    // No STRUCTURE_NOTIFY here: window structure (geometry/map state) is watched
    // on a separate connection by the geometry thread. Select these events on
    // the same xcb connection this thread polls; event masks are per-client.
    let mask = x::EventMask::KEY_PRESS
        | x::EventMask::KEY_RELEASE
        | x::EventMask::BUTTON_PRESS
        | x::EventMask::BUTTON_RELEASE
        | x::EventMask::POINTER_MOTION
        | x::EventMask::ENTER_WINDOW
        | x::EventMask::LEAVE_WINDOW;
    st.conn.send_request(&x::ChangeWindowAttributes {
        window: x::Window::new(st.window),
        value_list: &[x::Cw::EventMask(mask)],
    });
    let _ = st.conn.flush();

    let xcb_fd = st.conn.as_raw_fd();
    let shutdown_ev = x11_shutdown_waker();
    let shutdown_fd = unsafe { jfn_wake_event_fd(shutdown_ev) };

    let mut fds: [libc::pollfd; 2] = [
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
    ];

    loop {
        let rc = unsafe { libc::poll(fds.as_mut_ptr(), 2, -1) };
        if rc < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            break;
        }

        if fds[1].revents & libc::POLLIN != 0 {
            break;
        }
        if fds[0].revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
            break;
        }
        while let Ok(Some(ev)) = st.conn.poll_for_event() {
            handle_event(&mut st, ev);
        }
    }
}

fn cursor_thread_body(screen_num: i32, window: u32, mailbox: Arc<CursorMailbox>) {
    let Some(conn) = crate::x11_state::x11rb_conn() else {
        return;
    };
    let cursor_handle = new_from_default(&*conn).ok().and_then(|db| {
        X11rbCursorHandle::new(&*conn, screen_num as usize, &db)
            .ok()
            .and_then(|cookie| cookie.reply().ok())
    });
    if cursor_handle.is_none() {
        eprintln!("[x11] x11rb cursor handle creation failed");
    }

    let mut st = CursorState {
        conn,
        window,
        cache: std::collections::HashMap::new(),
        cursor_handle,
    };

    let mut fds = [libc::pollfd {
        fd: unsafe { jfn_wake_event_fd(mailbox.wake) },
        events: libc::POLLIN,
        revents: 0,
    }];

    loop {
        let rc = unsafe { libc::poll(fds.as_mut_ptr(), 1, -1) };
        if rc < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            break;
        }
        if fds[0].revents & libc::POLLIN == 0 {
            continue;
        }
        unsafe { jfn_wake_event_drain(mailbox.wake) };
        if mailbox.should_shutdown() {
            break;
        }
        drain_cursor_requests(&mut st, &mailbox);
    }
}

fn input_dispatch_thread_body(mailbox: Arc<InputMailbox>) {
    let mut fds = [libc::pollfd {
        fd: unsafe { jfn_wake_event_fd(mailbox.wake) },
        events: libc::POLLIN,
        revents: 0,
    }];

    loop {
        let rc = unsafe { libc::poll(fds.as_mut_ptr(), 1, -1) };
        if rc < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            break;
        }
        if fds[0].revents & libc::POLLIN == 0 {
            continue;
        }
        unsafe { jfn_wake_event_drain(mailbox.wake) };
        if mailbox.should_shutdown() {
            break;
        }
        for ev in mailbox.drain() {
            match ev {
                QueuedInputEvent::KeyRaw {
                    sym,
                    native,
                    modifiers,
                    pressed,
                } => jfn_input_dispatch_key_raw(sym, native, modifiers, pressed),
                QueuedInputEvent::Char {
                    cp,
                    modifiers,
                    native,
                } => jfn_input_dispatch_char(cp, modifiers, native),
                QueuedInputEvent::HistoryNav { forward } => jfn_input_dispatch_history_nav(forward),
                QueuedInputEvent::MouseButton {
                    code,
                    pressed,
                    x,
                    y,
                    modifiers,
                } => jfn_input_dispatch_mouse_button(code, pressed, x, y, modifiers),
                QueuedInputEvent::MouseMove {
                    x,
                    y,
                    modifiers,
                    leave,
                } => jfn_input_dispatch_mouse_move(x, y, modifiers, leave),
                QueuedInputEvent::Scroll {
                    x,
                    y,
                    dx,
                    dy,
                    modifiers,
                } => jfn_input_dispatch_scroll(x, y, dx, dy, modifiers),
            }
        }
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

pub fn start(screen_num: i32, parent: u32) -> Option<Handle> {
    let Some(conn) = crate::x11_state::xcb_conn() else {
        eprintln!("[x11] xcb input connection unavailable");
        return None;
    };
    let mailbox = Arc::new(CursorMailbox::new());
    let input_mailbox = Arc::new(InputMailbox::new());
    let (root, net_active_window) = crate::x11_state::MUT
        .lock()
        .as_ref()
        .map(|m| (m.root, m.atoms.net_active_window))
        .unwrap_or((0, 0));
    let st = State {
        conn: conn.clone(),
        window: parent,
        root,
        net_active_window,
        xkb_ctx: xkb::Context::new(xkb::CONTEXT_NO_FLAGS),
        xkb_kmap: None,
        xkb_st: None,
        xkb_device_id: -1,
        xkb_base_event: 0,
        modifiers: 0,
        ptr_x: 0,
        ptr_y: 0,
        mouse_button_modifiers: 0,
        mailbox: mailbox.clone(),
        input_mailbox: input_mailbox.clone(),
    };

    let input_thread_mailbox = input_mailbox.clone();
    let input_join = std::thread::Builder::new()
        .name("jfn-x11-input-dispatch".into())
        .spawn(move || input_dispatch_thread_body(input_thread_mailbox))
        .expect("spawn x11 input dispatch thread");

    let cursor_mailbox = mailbox.clone();
    let cursor_join = std::thread::Builder::new()
        .name("jfn-x11-cursor".into())
        .spawn(move || cursor_thread_body(screen_num, parent, cursor_mailbox))
        .expect("spawn x11 cursor thread");

    let join = std::thread::Builder::new()
        .name("jfn-x11-input".into())
        .spawn(move || input_thread_body(st))
        .expect("spawn x11 input thread");

    Some(Handle {
        join: Some(join),
        cursor_join: Some(cursor_join),
        input_join: Some(input_join),
        mailbox,
        input_mailbox,
    })
}

/// Capture pointer input directly on a WM-managed overlay.
///
/// Buttons go through a *passive grab* (`GrabButton`), not event selection,
/// because only one client may select `ButtonPress` on a window and the WM may
/// already hold it — a grab is independent of selection and cannot conflict.
/// Must use the same xcb connection the input thread polls.
pub fn grab_overlay_input(window: u32) {
    let Some(conn) = crate::x11_state::xcb_conn() else {
        return;
    };
    let w = x::Window::new(window);
    let mask =
        x::EventMask::POINTER_MOTION | x::EventMask::ENTER_WINDOW | x::EventMask::LEAVE_WINDOW;
    conn.send_request(&x::ChangeWindowAttributes {
        window: w,
        value_list: &[x::Cw::EventMask(mask)],
    });
    conn.send_request(&x::GrabButton {
        owner_events: true,
        grab_window: w,
        event_mask: x::EventMask::BUTTON_PRESS
            | x::EventMask::BUTTON_RELEASE
            | x::EventMask::POINTER_MOTION,
        pointer_mode: x::GrabMode::Async,
        keyboard_mode: x::GrabMode::Async,
        confine_to: x::Window::none(),
        cursor: x::Cursor::none(),
        button: x::ButtonIndex::Any,
        modifiers: x::ModMask::ANY,
    });
    let _ = conn.flush();
}

pub fn set_cursor(handle: &Handle, shape: CursorShape) {
    handle.mailbox.push(CursorReq::Set(shape));
}
