//! Wayland clipboard (CLIPBOARD selection) read path via ext-data-control-v1.
//!
//! Why not wl_data_device on the main display: wl_data_device is focus-bound,
//! and the main jellyfin wl_display competes with XWayland's clipboard bridge
//! on the same seat which CEF (running as an X11 ozone client) relies on for
//! Ctrl+V. ext-data-control-v1 is focus-independent, designed for clipboard
//! managers. Mirrors mpv's clipboard-wayland.c: dedicated wl_display_connect,
//! dedicated worker thread, no shared globals with the main display.

use std::ffi::{c_char, c_void};
use std::io::{ErrorKind, Read};
use std::os::fd::{AsFd, AsRawFd, FromRawFd, IntoRawFd, OwnedFd};
use std::os::raw::c_int;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};

use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{wl_registry, wl_seat};
use wayland_client::{event_created_child, Connection, Dispatch, Proxy, QueueHandle};
use wayland_protocols::ext::data_control::v1::client::{
    ext_data_control_device_v1::{self as dc_device, ExtDataControlDeviceV1},
    ext_data_control_manager_v1::ExtDataControlManagerV1,
    ext_data_control_offer_v1::{self as dc_offer, ExtDataControlOfferV1},
};

const MIME_TEXT_PLAIN_UTF8: &str = "text/plain;charset=utf-8";
const MIME_TEXT_PLAIN: &str = "text/plain";
const MIME_UTF8_STRING: &str = "UTF8_STRING";
const MIME_STRING: &str = "STRING";
const MIME_TEXT: &str = "TEXT";

#[derive(Default, Clone)]
struct OfferMimes {
    text_plain_utf8: bool,
    text_plain: bool,
    utf8_string: bool,
    string: bool,
    text: bool,
}

impl OfferMimes {
    fn best(&self) -> Option<&'static str> {
        if self.text_plain_utf8 {
            Some(MIME_TEXT_PLAIN_UTF8)
        } else if self.text_plain {
            Some(MIME_TEXT_PLAIN)
        } else if self.utf8_string {
            Some(MIME_UTF8_STRING)
        } else if self.string {
            Some(MIME_STRING)
        } else if self.text {
            Some(MIME_TEXT)
        } else {
            None
        }
    }
    fn observe(&mut self, mime: &str) {
        match mime {
            MIME_TEXT_PLAIN_UTF8 => self.text_plain_utf8 = true,
            MIME_TEXT_PLAIN => self.text_plain = true,
            MIME_UTF8_STRING => self.utf8_string = true,
            MIME_STRING => self.string = true,
            MIME_TEXT => self.text = true,
            _ => {}
        }
    }
}

type ReadCb = unsafe extern "C" fn(ctx: *mut c_void, text: *const c_char, len: usize);

struct PendingCb {
    cb: ReadCb,
    ctx: *mut c_void,
}

// Safety: the C side owns ctx lifetime and is responsible for thread safety,
// matching the C++ std::function semantics it replaces.
unsafe impl Send for PendingCb {}

struct Shared {
    queued: Mutex<Vec<PendingCb>>,
    stop: AtomicBool,
    wake_fd: c_int,
}

struct State {
    // Held to keep the Wayland proxies alive for the lifetime of the worker.
    #[allow(dead_code)]
    seat: Option<wl_seat::WlSeat>,
    #[allow(dead_code)]
    mgr: Option<ExtDataControlManagerV1>,
    device: Option<ExtDataControlDeviceV1>,
    // Pending mimes keyed by offer object id (an offer's mime set is built
    // up between data_offer and the selection event that takes it).
    pending_offer_mimes: std::collections::HashMap<u32, OfferMimes>,
    // Currently active selection offer + its mime set.
    current_offer: Option<(ExtDataControlOfferV1, OfferMimes)>,
}

pub struct JfnClipboardWayland {
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
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
        _: &mut Self,
        _: &wl_seat::WlSeat,
        _: wl_seat::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<ExtDataControlManagerV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &ExtDataControlManagerV1,
        _: <ExtDataControlManagerV1 as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<ExtDataControlDeviceV1, ()> for State {
    fn event(
        state: &mut Self,
        _: &ExtDataControlDeviceV1,
        event: dc_device::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            dc_device::Event::DataOffer { id } => {
                state.pending_offer_mimes.insert(id.id().protocol_id(), OfferMimes::default());
            }
            dc_device::Event::Selection { id } => {
                if let Some((prev, _)) = state.current_offer.take() {
                    prev.destroy();
                }
                if let Some(offer) = id {
                    let key = offer.id().protocol_id();
                    let mimes = state.pending_offer_mimes.remove(&key).unwrap_or_default();
                    state.current_offer = Some((offer, mimes));
                }
            }
            dc_device::Event::Finished => {
                if let Some(dev) = state.device.take() {
                    dev.destroy();
                }
            }
            dc_device::Event::PrimarySelection { id } => {
                if let Some(offer) = id {
                    state.pending_offer_mimes.remove(&offer.id().protocol_id());
                    offer.destroy();
                }
            }
            _ => {}
        }
    }

    event_created_child!(State, ExtDataControlDeviceV1, [
        dc_device::EVT_DATA_OFFER_OPCODE => (ExtDataControlOfferV1, ()),
        dc_device::EVT_PRIMARY_SELECTION_OPCODE => (ExtDataControlOfferV1, ()),
    ]);
}

impl Dispatch<ExtDataControlOfferV1, ()> for State {
    fn event(
        state: &mut Self,
        offer: &ExtDataControlOfferV1,
        event: dc_offer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let dc_offer::Event::Offer { mime_type } = event {
            let key = offer.id().protocol_id();
            if let Some(mimes) = state.pending_offer_mimes.get_mut(&key) {
                mimes.observe(&mime_type);
            } else if let Some((cur, mimes)) = state.current_offer.as_mut() {
                if cur.id().protocol_id() == key {
                    mimes.observe(&mime_type);
                }
            }
        }
    }
}

fn make_wake_fd() -> Option<c_int> {
    let fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    if fd < 0 { None } else { Some(fd) }
}

fn signal_wake(fd: c_int) {
    let v: u64 = 1;
    unsafe {
        libc::write(fd, &v as *const u64 as *const c_void, 8);
    }
}

fn drain_wake(fd: c_int) {
    let mut buf = [0u8; 64];
    loop {
        let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut c_void, buf.len()) };
        if n <= 0 {
            break;
        }
    }
}

fn fire(cb: PendingCb, text: &[u8]) {
    unsafe {
        (cb.cb)(cb.ctx, text.as_ptr() as *const c_char, text.len());
    }
}

fn start_receive(state: &mut State, conn: &Connection) -> Option<OwnedFd> {
    let (offer, mimes) = state.current_offer.as_ref()?;
    let mime = mimes.best()?;
    let mut fds: [c_int; 2] = [-1, -1];
    if unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_CLOEXEC | libc::O_NONBLOCK) } < 0 {
        return None;
    }
    let read_end = unsafe { OwnedFd::from_raw_fd(fds[0]) };
    let write_end = unsafe { OwnedFd::from_raw_fd(fds[1]) };
    offer.receive(mime.to_owned(), write_end.as_fd());
    let _ = conn.flush();
    drop(write_end);
    Some(read_end)
}

fn worker_loop(
    shared: Arc<Shared>,
    conn: Connection,
    mut queue: wayland_client::EventQueue<State>,
    mut state: State,
) {
    let display_fd = conn.as_fd().as_raw_fd();
    let wake_fd = shared.wake_fd;

    // (read_fd, callback, buffer) for the in-flight receive — at most one
    // active at a time, matching the C++ implementation's natural
    // back-pressure model.
    let mut active: Option<(OwnedFd, PendingCb, Vec<u8>)> = None;

    while !shared.stop.load(Ordering::Relaxed) {
        // Drain anything already buffered before preparing a new read.
        let _ = queue.dispatch_pending(&mut state);
        let _ = conn.flush();

        let read_guard = match queue.prepare_read() {
            Some(g) => g,
            None => continue,
        };

        let mut pfds: Vec<libc::pollfd> = Vec::with_capacity(3);
        pfds.push(libc::pollfd { fd: display_fd, events: libc::POLLIN, revents: 0 });
        pfds.push(libc::pollfd { fd: wake_fd, events: libc::POLLIN, revents: 0 });
        if let Some((fd, _, _)) = &active {
            pfds.push(libc::pollfd { fd: fd.as_raw_fd(), events: libc::POLLIN, revents: 0 });
        }

        let r = unsafe { libc::poll(pfds.as_mut_ptr(), pfds.len() as _, -1) };
        if r < 0 {
            let err = std::io::Error::last_os_error();
            drop(read_guard);
            if err.kind() == ErrorKind::Interrupted {
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
            drain_wake(wake_fd);
            let _ = queue.dispatch_pending(&mut state);
        }

        // Active receive.
        if let Some((fd, _, buf)) = active.as_mut() {
            if pfds.len() > 2 {
                let revents = pfds[2].revents;
                let mut done = false;
                if revents & libc::POLLIN != 0 {
                    let mut tmp = [0u8; 4096];
                    let mut file = unsafe { std::fs::File::from_raw_fd(fd.as_raw_fd()) };
                    loop {
                        match file.read(&mut tmp) {
                            Ok(0) => {
                                done = true;
                                break;
                            }
                            Ok(n) => buf.extend_from_slice(&tmp[..n]),
                            Err(e) if e.kind() == ErrorKind::WouldBlock => break,
                            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                            Err(_) => {
                                done = true;
                                break;
                            }
                        }
                    }
                    // Don't let File drop close the fd — it's owned by OwnedFd above.
                    let _ = file.into_raw_fd();
                }
                if revents & (libc::POLLHUP | libc::POLLERR | libc::POLLNVAL) != 0 {
                    done = true;
                }
                if done {
                    let (_, cb, buf) = active.take().unwrap();
                    fire(cb, &buf);
                }
            }
        }

        // Promote the next queued request if the active slot is free.
        if active.is_none() {
            let next = {
                let mut q = shared.queued.lock().unwrap();
                if q.is_empty() {
                    None
                } else {
                    Some(q.remove(0))
                }
            };
            if let Some(cb) = next {
                match start_receive(&mut state, &conn) {
                    Some(fd) => active = Some((fd, cb, Vec::new())),
                    None => {
                        fire(cb, &[]);
                        // Anything else queued has the same problem (no offer,
                        // no text mime, pipe failure) — drain with empty results.
                        let drained: Vec<PendingCb> = {
                            let mut q = shared.queued.lock().unwrap();
                            std::mem::take(&mut *q)
                        };
                        for cb in drained {
                            fire(cb, &[]);
                        }
                    }
                }
            }
        }

        let _ = queue.dispatch_pending(&mut state);
    }

    if let Some((_, cb, _)) = active.take() {
        fire(cb, &[]);
    }
    let drained: Vec<PendingCb> = {
        let mut q = shared.queued.lock().unwrap();
        std::mem::take(&mut *q)
    };
    for cb in drained {
        fire(cb, &[]);
    }
}

fn init_impl() -> Option<JfnClipboardWayland> {
    let conn = Connection::connect_to_env().ok()?;
    let (globals, mut queue) = registry_queue_init::<State>(&conn).ok()?;
    let qh = queue.handle();

    let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=8, ()).ok()?;
    let mgr: ExtDataControlManagerV1 = globals.bind(&qh, 1..=1, ()).ok()?;
    let device = mgr.get_data_device(&seat, &qh, ());

    let mut state = State {
        seat: Some(seat),
        mgr: Some(mgr),
        device: Some(device),
        pending_offer_mimes: Default::default(),
        current_offer: None,
    };
    queue.roundtrip(&mut state).ok()?;

    let wake_fd = make_wake_fd()?;
    let shared = Arc::new(Shared {
        queued: Mutex::new(Vec::new()),
        stop: AtomicBool::new(false),
        wake_fd,
    });
    let shared_w = shared.clone();
    let worker = thread::spawn(move || worker_loop(shared_w, conn, queue, state));
    Some(JfnClipboardWayland {
        shared,
        worker: Some(worker),
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_clipboard_wayland_init() -> *mut JfnClipboardWayland {
    match init_impl() {
        Some(c) => Box::into_raw(Box::new(c)),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `c` must be a pointer previously returned by `jfn_clipboard_wayland_init`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_clipboard_wayland_read_text_async(
    c: *mut JfnClipboardWayland,
    cb: Option<ReadCb>,
    ctx: *mut c_void,
) {
    let Some(c) = (unsafe { c.as_ref() }) else { return };
    let Some(cb) = cb else { return };
    {
        let mut q = c.shared.queued.lock().unwrap();
        q.push(PendingCb { cb, ctx });
    }
    signal_wake(c.shared.wake_fd);
}

/// # Safety
/// `c` must be a pointer previously returned by `jfn_clipboard_wayland_init`,
/// or null. Consumes the box.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_clipboard_wayland_cleanup(c: *mut JfnClipboardWayland) {
    if c.is_null() {
        return;
    }
    let mut boxed = unsafe { Box::from_raw(c) };
    boxed.shared.stop.store(true, Ordering::Relaxed);
    signal_wake(boxed.shared.wake_fd);
    if let Some(w) = boxed.worker.take() {
        let _ = w.join();
    }
    unsafe { libc::close(boxed.shared.wake_fd) };
}
