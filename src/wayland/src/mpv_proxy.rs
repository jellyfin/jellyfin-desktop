//! Wayland proxy between mpv and the compositor.
//!
//! mpv connects here instead of the real compositor (via WAYLAND_DISPLAY env).
//! Messages forward in both directions; selected requests are intercepted.
//!
//! We don't use `SimpleProxy` because it builds each per-client `State` using
//! the current process `WAYLAND_DISPLAY` env to find the upstream compositor —
//! but the caller overrides that env to OUR socket so mpv connects to us. We
//! must capture the original `WAYLAND_DISPLAY` here at `start` (before any
//! override) and pass it explicitly via `with_server_display_name`.

use std::cell::RefCell;
use std::ffi::CString;
use std::os::fd::{AsRawFd, IntoRawFd, OwnedFd};
use std::os::raw::{c_char, c_int};
use std::rc::Rc;
use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};
use std::sync::mpsc;
use std::thread;

use error_reporter::Report;
use jfn_wake_event::WakeEvent;
use parking_lot::Mutex;
use wl_proxy::baseline::Baseline;
use wl_proxy::client::{Client, ClientHandler};
use wl_proxy::object::{ConcreteObject, Object, ObjectCoreApi, ObjectError, ObjectRcUtils};
use wl_proxy::protocols::ObjectInterface;
use wl_proxy::protocols::fractional_scale_v1::wp_fractional_scale_manager_v1::{
    WpFractionalScaleManagerV1, WpFractionalScaleManagerV1Handler,
};
use wl_proxy::protocols::fractional_scale_v1::wp_fractional_scale_v1::{
    WpFractionalScaleV1, WpFractionalScaleV1Handler,
};
use wl_proxy::protocols::linux_drm_syncobj_v1::wp_linux_drm_syncobj_manager_v1::WpLinuxDrmSyncobjManagerV1;
use wl_proxy::protocols::viewporter::wp_viewport::{WpViewport, WpViewportHandler};
use wl_proxy::protocols::viewporter::wp_viewporter::{WpViewporter, WpViewporterHandler};
use wl_proxy::protocols::wayland::wl_callback::{WlCallback, WlCallbackHandler};
use wl_proxy::protocols::wayland::wl_compositor::WlCompositor;
use wl_proxy::protocols::wayland::wl_display::{WlDisplay, WlDisplayHandler};
use wl_proxy::protocols::wayland::wl_keyboard::{
    WlKeyboard, WlKeyboardHandler, WlKeyboardKeyState,
};
use wl_proxy::protocols::wayland::wl_output::WlOutput;
use wl_proxy::protocols::wayland::wl_pointer::{WlPointer, WlPointerButtonState, WlPointerHandler};
use wl_proxy::protocols::wayland::wl_region::WlRegion;
use wl_proxy::protocols::wayland::wl_registry::{WlRegistry, WlRegistryHandler};
use wl_proxy::protocols::wayland::wl_seat::{WlSeat, WlSeatHandler};
use wl_proxy::protocols::wayland::wl_subcompositor::WlSubcompositor;
use wl_proxy::protocols::wayland::wl_subsurface::WlSubsurface;
use wl_proxy::protocols::wayland::wl_surface::{WlSurface, WlSurfaceHandler};
use wl_proxy::protocols::wayland::wl_touch::WlTouch;
use wl_proxy::protocols::xdg_shell::xdg_surface::{XdgSurface, XdgSurfaceHandler};
use wl_proxy::protocols::xdg_shell::xdg_toplevel::{
    XdgToplevel, XdgToplevelHandler, XdgToplevelState,
};
use wl_proxy::protocols::xdg_shell::xdg_wm_base::{XdgWmBase, XdgWmBaseHandler};
use wl_proxy::state::State;

use crate::window_state::WindowSize;

pub struct Proxy {
    display_name: CString,
    _app_thread: thread::JoinHandle<()>,
    _mpv_thread: thread::JoinHandle<()>,
}

static CUR_W: AtomicI32 = AtomicI32::new(0);
static CUR_H: AtomicI32 = AtomicI32::new(0);
static WINDOW_SIZE_GEN: AtomicU32 = AtomicU32::new(0);

// S_mpv records this from `server_id()`; S_app matches it against `client_id()`
// on client M. Same wire object => the two ids are equal.
static MPV_VIDEO_SURFACE_ID: AtomicU32 = AtomicU32::new(0);

static APP_CLIENT_FD: AtomicI32 = AtomicI32::new(-1);

pub(crate) fn app_client_fd() -> Option<std::os::fd::RawFd> {
    let fd = APP_CLIENT_FD.load(Ordering::Acquire);
    (fd >= 0).then_some(fd)
}

/// The one authoritative window size, published by the host's configure via
/// [`set_window_size`]. `None` until the host toplevel has been configured —
/// there is no boot/default to fall back to, so mpv can only ever mirror the
/// real window geometry.
fn window_size() -> Option<WindowSize> {
    WindowSize::new(CUR_W.load(Ordering::Acquire), CUR_H.load(Ordering::Acquire))
}

/// Owns the S_mpv thread's wake eventfd. Writers signal, and teardown clears it,
/// under this lock — so a wake can never write to a descriptor that has been
/// closed (and possibly reused) out from under it.
static MPV_WAKE: Mutex<Option<WakeEvent>> = Mutex::new(None);

pub(crate) fn set_window_size(size: WindowSize) {
    CUR_W.store(size.w(), Ordering::Release);
    CUR_H.store(size.h(), Ordering::Release);
    WINDOW_SIZE_GEN.fetch_add(1, Ordering::AcqRel);
    wake_mpv_thread();
}

fn wake_mpv_thread() {
    if let Some(wake) = MPV_WAKE.lock().as_ref() {
        wake.signal();
    }
}

thread_local! {
    static SHELL: RefCell<Shell> = const { RefCell::new(Shell::new()) };
}

struct Shell {
    display: Option<Rc<WlDisplay>>,
    client: Option<Rc<Client>>,
    compositor: Option<Rc<WlCompositor>>,
    subcompositor: Option<Rc<WlSubcompositor>>,
    wm_base: Option<Rc<XdgWmBase>>,
    globals_ready: bool,
    roundtrip_started: bool,
    host_root_surface: Option<Rc<WlSurface>>,
    host_root_xdg_surface: Option<Rc<XdgSurface>>,
    spliced: bool,
    mpv_client: Option<Rc<Client>>,
    configurator: Option<MpvConfigurator>,
    mpv_subsurface: Option<SyncSubsurface>,
    serial: u32,
}

/// mpv's xdg objects, present only once mpv has created its toplevel. Holding
/// both together makes "emit a configure without a toplevel" unrepresentable:
/// a configure can only be sent through an `MpvConfigurator`, which exists only
/// after `get_toplevel`.
#[derive(Clone)]
struct MpvConfigurator {
    toplevel: Rc<XdgToplevel>,
    xdg_surface: Rc<XdgSurface>,
}

impl MpvConfigurator {
    fn configure(&self, size: WindowSize, serial: u32, states: &[u8]) {
        if let Err(e) = self.toplevel.try_send_configure(size.w(), size.h(), states) {
            tracing::error!(target: "MpvProxy", "synth toplevel configure: {}", Report::new(&e));
        }
        if let Err(e) = self.xdg_surface.try_send_configure(serial) {
            tracing::error!(target: "MpvProxy", "synth xdg_surface configure: {}", Report::new(&e));
        }
    }
}

/// A subsurface kept permanently in Wayland synchronized mode: its buffer,
/// viewport and position apply atomically on the parent surface's commit. The
/// raw object never escapes and no `set_desync` is exposed, so a desynchronized
/// video layer — one that could present a size the window does not have — is
/// unrepresentable.
struct SyncSubsurface(Rc<WlSubsurface>);

impl SyncSubsurface {
    fn create(
        subcompositor: &Rc<WlSubcompositor>,
        surface: &Rc<WlSurface>,
        parent: &Rc<WlSurface>,
    ) -> Result<Self, ObjectError> {
        let sub = subcompositor.create_child::<WlSubsurface>();
        // Born synchronized (the protocol default); never desynced.
        subcompositor.try_send_get_subsurface(&sub, surface, parent)?;
        Ok(Self(sub))
    }

    fn set_position(&self, x: i32, y: i32) -> Result<(), ObjectError> {
        self.0.try_send_set_position(x, y)
    }

    fn place_above(&self, sibling: &Rc<WlSurface>) -> Result<(), ObjectError> {
        self.0.try_send_place_above(sibling)
    }
}

impl Shell {
    const fn new() -> Self {
        Self {
            display: None,
            client: None,
            compositor: None,
            subcompositor: None,
            wm_base: None,
            globals_ready: false,
            roundtrip_started: false,
            host_root_surface: None,
            host_root_xdg_surface: None,
            spliced: false,
            mpv_client: None,
            configurator: None,
            mpv_subsurface: None,
            serial: 0,
        }
    }

    fn next_serial(&mut self) -> u32 {
        self.serial = self.serial.wrapping_add(1);
        self.serial
    }
}

fn with_shell<R>(f: impl FnOnce(&mut Shell) -> R) -> R {
    SHELL.with(|s| f(&mut s.borrow_mut()))
}

/// Forwarding sends can't unwind a handler; a failure desyncs a single message
/// but is unrecoverable in place, so surface it through our infra and continue.
fn log_send(op: &str, res: Result<(), wl_proxy::object::ObjectError>) {
    if let Err(e) = res {
        tracing::warn!(target: "MpvProxy", "{op}: {}", Report::new(&e));
    }
}

pub fn start() -> *mut Proxy {
    // Capture upstream BEFORE the caller overrides WAYLAND_DISPLAY, so S_app
    // connects to the real compositor rather than our own socket.
    let upstream = std::env::var("WAYLAND_DISPLAY").ok();

    let (sp_a, sp_b) = match socketpair_cloexec() {
        Ok(p) => p,
        Err(e) => {
            eprintln!("proxy: socketpair: {e}");
            return std::ptr::null_mut();
        }
    };

    let (tx_app, rx_app) = mpsc::sync_channel::<Result<OwnedFd, String>>(1);
    let app_thread = match thread::Builder::new()
        .name("proxy-app".into())
        .spawn(move || run_app_state(tx_app, upstream, sp_b))
    {
        Ok(h) => h,
        Err(e) => {
            eprintln!("proxy: app thread spawn failed: {e}");
            return std::ptr::null_mut();
        }
    };
    match rx_app.recv() {
        Ok(Ok(app_fd)) => {
            APP_CLIENT_FD.store(app_fd.into_raw_fd(), Ordering::Release);
        }
        Ok(Err(msg)) => {
            eprintln!("proxy: {msg}");
            return std::ptr::null_mut();
        }
        Err(_) => {
            eprintln!("proxy: app thread exited before publishing client fd");
            return std::ptr::null_mut();
        }
    }

    let (tx_mpv, rx_mpv) = mpsc::sync_channel::<Result<CString, String>>(1);
    let mpv_thread = match thread::Builder::new()
        .name("proxy-mpv".into())
        .spawn(move || run_mpv_state(tx_mpv, sp_a))
    {
        Ok(h) => h,
        Err(e) => {
            eprintln!("proxy: mpv thread spawn failed: {e}");
            return std::ptr::null_mut();
        }
    };
    let display_name = match rx_mpv.recv() {
        Ok(Ok(n)) => n,
        Ok(Err(msg)) => {
            eprintln!("proxy: {msg}");
            return std::ptr::null_mut();
        }
        Err(_) => {
            eprintln!("proxy: mpv thread exited before sending display name");
            return std::ptr::null_mut();
        }
    };
    Box::into_raw(Box::new(Proxy {
        display_name,
        _app_thread: app_thread,
        _mpv_thread: mpv_thread,
    }))
}

fn socketpair_cloexec() -> std::io::Result<(OwnedFd, OwnedFd)> {
    use std::os::fd::FromRawFd;
    let mut fds = [0 as c_int; 2];
    let rc = unsafe {
        libc::socketpair(
            libc::AF_UNIX,
            libc::SOCK_STREAM | libc::SOCK_CLOEXEC,
            0,
            fds.as_mut_ptr(),
        )
    };
    if rc != 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(unsafe { (OwnedFd::from_raw_fd(fds[0]), OwnedFd::from_raw_fd(fds[1])) })
}

/// Returns the WAYLAND_DISPLAY value clients should connect to (e.g. "wayland-1").
/// Returns null if `p` is null. Pointer is valid until `stop`.
///
/// # Safety
/// `p` must be null or a pointer previously returned by `start`
/// that has not yet been passed to `stop`.
pub unsafe fn display_name(p: *const Proxy) -> *const c_char {
    if p.is_null() {
        return std::ptr::null();
    }
    unsafe { (*p).display_name.as_ptr() }
}

/// Drop the proxy handle. The listener thread is detached; OS cleans up on
/// process exit. Safe to call with null.
///
/// # Safety
/// `p` must be null or a pointer previously returned by `start`.
/// Each non-null pointer may only be passed here once.
pub unsafe fn stop(p: *mut Proxy) {
    if p.is_null() {
        return;
    }
    unsafe { drop(Box::from_raw(p)) };
}

fn run_app_state(
    tx: mpsc::SyncSender<Result<OwnedFd, String>>,
    upstream: Option<String>,
    mpv_bridge: OwnedFd,
) {
    let mut builder = State::builder(Baseline::ALL_OF_THEM).with_log_prefix("jfn-app");
    if let Some(name) = &upstream {
        builder = builder.with_server_display_name(name);
    }
    let state = match builder.build() {
        Ok(s) => s,
        Err(e) => {
            let _ = tx.send(Err(format!("S_app build: {}", Report::new(e))));
            return;
        }
    };

    let (client_a, app_fd) = match state.connect() {
        Ok(ca) => ca,
        Err(e) => {
            let _ = tx.send(Err(format!("S_app connect: {}", Report::new(e))));
            return;
        }
    };
    client_a.set_handler(NoopClient);
    client_a.display().set_handler(AppDisplayH);
    with_shell(|sh| sh.client = Some(client_a.clone()));
    if tx.send(Ok(app_fd)).is_err() {
        return;
    }
    drop(tx);

    let client_m = match state.add_client(&Rc::new(mpv_bridge)) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("proxy: S_app add mpv bridge: {}", Report::new(e));
            return;
        }
    };
    client_m.set_handler(NoopClient);
    client_m.display().set_handler(ForwardDisplayH);
    with_shell(|sh| sh.mpv_client = Some(client_m.clone()));

    while state.is_not_destroyed() {
        match state.dispatch(None) {
            Ok(_) => {}
            Err(e) => {
                eprintln!("proxy: S_app dispatch: {}", Report::new(e));
                return;
            }
        }
        ensure_root();
        maybe_build_root();
    }
}

fn run_mpv_state(tx: mpsc::SyncSender<Result<CString, String>>, bridge: OwnedFd) {
    let state = match State::builder(Baseline::ALL_OF_THEM)
        .with_log_prefix("jfn-mpv")
        .with_server_fd(&Rc::new(bridge))
        .build()
    {
        Ok(s) => s,
        Err(e) => {
            let _ = tx.send(Err(format!("S_mpv build: {}", Report::new(e))));
            return;
        }
    };
    let acceptor = match state.create_acceptor(1000) {
        Ok(a) => a,
        Err(e) => {
            let _ = tx.send(Err(format!("S_mpv acceptor: {}", Report::new(e))));
            return;
        }
    };
    let name = match CString::new(acceptor.display()) {
        Ok(s) => s,
        Err(e) => {
            let _ = tx.send(Err(format!("display name has NUL: {e}")));
            return;
        }
    };
    if tx.send(Ok(name)).is_err() {
        return;
    }
    drop(tx);
    state.set_handler(MpvShimStateH);

    let Some(wake) = WakeEvent::new() else {
        eprintln!("proxy: S_mpv eventfd failed");
        return;
    };
    // Cache the raw fd for this thread's own poll/drain; cross-thread wakes go
    // through MPV_WAKE under its lock, never this number.
    let wake_fd = wake.fd();
    *MPV_WAKE.lock() = Some(wake);
    // Clear (and close) the wake handle under the same lock a wake signals under.
    struct WakeGuard;
    impl Drop for WakeGuard {
        fn drop(&mut self) {
            MPV_WAKE.lock().take();
        }
    }
    let _wake_guard = WakeGuard;

    // Block on both the mpv connection and the size-feed wake fd so a resize
    // reaches mpv without waiting out a poll timeout.
    let poll_fd = state.poll_fd().as_raw_fd();
    let mut seen_gen = 0;
    while state.is_not_destroyed() {
        // Reconcile before every blocking poll, not only after dispatch: a size
        // raised before the wake handle was published (line above) delivers no
        // wake, so servicing it here is the only thing that keeps it from
        // sleeping out an infinite poll timeout on an otherwise-idle connection.
        apply_window_size_mpv(&mut seen_gen);
        if let Err(e) = state.before_poll() {
            eprintln!("proxy: S_mpv before_poll: {}", Report::new(e));
            return;
        }
        let mut pfds = [
            libc::pollfd {
                fd: poll_fd,
                events: libc::POLLIN,
                revents: 0,
            },
            libc::pollfd {
                fd: wake_fd,
                events: libc::POLLIN,
                revents: 0,
            },
        ];
        let r = unsafe { libc::poll(pfds.as_mut_ptr(), pfds.len() as _, -1) };
        if r < 0 {
            let err = std::io::Error::last_os_error();
            if err.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            eprintln!("proxy: S_mpv poll: {err}");
            return;
        }
        if pfds[1].revents & libc::POLLIN != 0 {
            jfn_wake_event::drain_raw_fd(wake_fd);
        }
        if let Err(e) = state.dispatch_available() {
            eprintln!("proxy: S_mpv dispatch: {}", Report::new(e));
            return;
        }
    }
}

fn apply_window_size_mpv(seen_gen: &mut u32) {
    let cur_gen = WINDOW_SIZE_GEN.load(Ordering::Acquire);
    if cur_gen == *seen_gen {
        return;
    }
    let Some(size) = window_size() else {
        return;
    };
    // Advance the marker only once a configure is actually emitted; before mpv's
    // toplevel exists this defers (retried each tick), it never guesses.
    if emit_mpv_configure(size) {
        *seen_gen = cur_gen;
    }
}

/// MAXIMIZED puts mpv in `locked_size`, so mpv holds the size we hand it instead
/// of re-deriving its geometry from the video.
const LOCKED_STATES: [u8; size_of::<u32>()] = XdgToplevelState::MAXIMIZED.0.to_ne_bytes();

/// Emit a configure to mpv iff its toplevel exists, returning whether one was
/// sent. A configure can only be built from an `MpvConfigurator`, so a
/// toplevel-less emit is unrepresentable rather than guarded.
fn emit_mpv_configure(size: WindowSize) -> bool {
    let emit = with_shell(|sh| {
        let cfg = sh.configurator.clone()?;
        Some((cfg, sh.next_serial()))
    });
    match emit {
        Some((cfg, serial)) => {
            cfg.configure(size, serial, &LOCKED_STATES);
            true
        }
        None => false,
    }
}

struct MpvShimStateH;
impl wl_proxy::state::StateHandler for MpvShimStateH {
    fn new_client(&mut self, client: &Rc<Client>) {
        client.set_handler(NoopClient);
        client.display().set_handler(MpvDisplayH);
    }
}

struct ForwardDisplayH;
impl WlDisplayHandler for ForwardDisplayH {
    fn handle_get_registry(&mut self, slf: &Rc<WlDisplay>, registry: &Rc<WlRegistry>) {
        log_send(
            "wl_display.get_registry",
            slf.try_send_get_registry(registry),
        );
    }
}

struct NoopClient;
impl ClientHandler for NoopClient {
    fn disconnected(self: Box<Self>) {}
}

struct AppDisplayH;
impl WlDisplayHandler for AppDisplayH {
    fn handle_get_registry(&mut self, slf: &Rc<WlDisplay>, registry: &Rc<WlRegistry>) {
        with_shell(|sh| {
            if sh.display.is_none() {
                sh.display = Some(slf.clone());
            }
        });
        registry.set_handler(AppRegistryH);
        log_send(
            "wl_display.get_registry",
            slf.try_send_get_registry(registry),
        );
    }
}

struct AppRegistryH;
impl WlRegistryHandler for AppRegistryH {
    fn handle_bind(&mut self, slf: &Rc<WlRegistry>, name: u32, id: Rc<dyn Object>) {
        match id.interface() {
            XdgWmBase::INTERFACE => {
                id.downcast::<XdgWmBase>().set_handler(AppWmBaseH);
            }
            WpFractionalScaleManagerV1::INTERFACE => {
                id.downcast::<WpFractionalScaleManagerV1>()
                    .set_handler(FracScaleMgrH);
            }
            WlSeat::INTERFACE => {
                id.downcast::<WlSeat>().set_handler(ForwardSeatH);
            }
            _ => {}
        }
        log_send("wl_registry.bind", slf.try_send_bind(name, id));
    }

    fn handle_global(
        &mut self,
        slf: &Rc<WlRegistry>,
        name: u32,
        interface: ObjectInterface,
        version: u32,
    ) {
        // Hide explicit-sync from the app's virtual registry as well. If the
        // compositor advertises `wp_linux_drm_syncobj_manager_v1` to the app,
        // the app's GPU paths (CEF / gpu_paint_worker) may bind it and trigger
        // the same driver-level race. Preventing the app from ever seeing this
        // global forces implicit sync for its surfaces too.
        if interface == WpLinuxDrmSyncobjManagerV1::INTERFACE {
            return;
        }
        log_send(
            "wl_registry.global",
            slf.try_send_global(name, interface, version),
        );
    }
}

struct MpvDisplayH;
impl WlDisplayHandler for MpvDisplayH {
    fn handle_get_registry(&mut self, slf: &Rc<WlDisplay>, registry: &Rc<WlRegistry>) {
        with_shell(|sh| {
            if sh.display.is_none() {
                sh.display = Some(slf.clone());
            }
        });
        registry.set_handler(MpvRegistryH);
        log_send(
            "wl_display.get_registry",
            slf.try_send_get_registry(registry),
        );
    }
}

struct MpvRegistryH;
impl WlRegistryHandler for MpvRegistryH {
    fn handle_global(
        &mut self,
        slf: &Rc<WlRegistry>,
        name: u32,
        interface: ObjectInterface,
        version: u32,
    ) {
        // Hide explicit-sync from mpv's virtual client entirely. mpv's Vulkan
        // WSI (via libplacebo) recreates its swapchain on every xdg-driven
        // resize with no coordination with the compositor's linux-drm-syncobj
        // timeline bookkeeping; under a rapid resize burst this has been
        // observed to race the compositor into a fatal "Missing buffer"
        // protocol error (a commit landing without a buffer while a sync
        // timeline point is still pending), which is fatal to the whole
        // shared real connection. Never advertising this global to mpv means
        // its WSI never requests it and falls back to implicit sync, which
        // has no such race.
        if interface == WpLinuxDrmSyncobjManagerV1::INTERFACE {
            return;
        }
        log_send(
            "wl_registry.global",
            slf.try_send_global(name, interface, version),
        );
    }

    fn handle_bind(&mut self, slf: &Rc<WlRegistry>, name: u32, id: Rc<dyn Object>) {
        match id.interface() {
            XdgWmBase::INTERFACE => {
                id.downcast::<XdgWmBase>().set_handler(MpvWmBaseH);
            }
            WpFractionalScaleManagerV1::INTERFACE => {
                id.downcast::<WpFractionalScaleManagerV1>()
                    .set_handler(FracScaleMgrH);
            }
            WlSeat::INTERFACE => {
                id.downcast::<WlSeat>().set_handler(BlockSeatH);
            }
            WpViewporter::INTERFACE => {
                id.downcast::<WpViewporter>().set_handler(ClientViewporterH);
            }
            _ => {}
        }
        log_send("wl_registry.bind", slf.try_send_bind(name, id));
    }
}

struct ClientViewporterH;
impl WpViewporterHandler for ClientViewporterH {
    fn handle_get_viewport(
        &mut self,
        slf: &Rc<WpViewporter>,
        id: &Rc<WpViewport>,
        surface: &Rc<WlSurface>,
    ) {
        id.set_handler(ClientViewportH);
        log_send(
            "wp_viewporter.get_viewport",
            slf.try_send_get_viewport(id, surface),
        );
    }
}

struct ClientViewportH;
impl WpViewportHandler for ClientViewportH {
    fn handle_set_source(
        &mut self,
        slf: &Rc<WpViewport>,
        x: wl_proxy::fixed::Fixed,
        y: wl_proxy::fixed::Fixed,
        width: wl_proxy::fixed::Fixed,
        height: wl_proxy::fixed::Fixed,
    ) {
        // Mirrors handle_set_destination below: an invalid source rect is an
        // instant, connection-fatal protocol error (bad_value), so it must be
        // dropped rather than forwarded verbatim. The unset form is
        // (-1, -1, -1, -1); any other non-positive width/height or negative
        // x/y is invalid.
        let neg_one = wl_proxy::fixed::Fixed::from_i32_saturating(-1);
        let zero = wl_proxy::fixed::Fixed::ZERO;
        let unset = x == neg_one && y == neg_one && width == neg_one && height == neg_one;
        if !unset && (x < zero || y < zero || width <= zero || height <= zero) {
            return;
        }
        log_send(
            "wp_viewport.set_source",
            slf.try_send_set_source(x, y, width, height),
        );
    }

    fn handle_set_destination(&mut self, slf: &Rc<WpViewport>, width: i32, height: i32) {
        // Virtualizing mpv's shell means it can size a viewport before it has a
        // real geometry, emitting a transient set_destination(0,0) — an instant
        // protocol error that would kill the shared connection. Drop non-positive
        // destinations (the unset form is -1,-1); mpv re-sizes once it has
        // geometry from our synthesized configure.
        let unset = width == -1 && height == -1;
        if !unset && (width <= 0 || height <= 0) {
            return;
        }
        // Forward mpv's own destination rect unchanged: mpv is pinned to our
        // window size by the locked-state configure and letterboxes internally,
        // so overriding this rect with the window size stretches the video.
        log_send(
            "wp_viewport.set_destination",
            slf.try_send_set_destination(width, height),
        );
    }
}

struct FracScaleMgrH;
impl WpFractionalScaleManagerV1Handler for FracScaleMgrH {
    fn handle_get_fractional_scale(
        &mut self,
        slf: &Rc<WpFractionalScaleManagerV1>,
        id: &Rc<WpFractionalScaleV1>,
        surface: &Rc<WlSurface>,
    ) {
        id.set_handler(FracScaleH);
        log_send(
            "wp_fractional_scale_manager_v1.get_fractional_scale",
            slf.try_send_get_fractional_scale(id, surface),
        );
    }
}

struct FracScaleH;
impl WpFractionalScaleV1Handler for FracScaleH {
    fn handle_preferred_scale(&mut self, slf: &Rc<WpFractionalScaleV1>, scale: u32) {
        log_send(
            "wp_fractional_scale_v1.preferred_scale",
            slf.try_send_preferred_scale(scale),
        );
    }
}

struct ForwardSeatH;
impl WlSeatHandler for ForwardSeatH {
    fn handle_get_pointer(&mut self, slf: &Rc<WlSeat>, id: &Rc<WlPointer>) {
        id.set_handler(PointerH);
        log_send("wl_seat.get_pointer", slf.try_send_get_pointer(id));
    }
    fn handle_get_keyboard(&mut self, slf: &Rc<WlSeat>, id: &Rc<WlKeyboard>) {
        id.set_handler(KeyboardH);
        log_send("wl_seat.get_keyboard", slf.try_send_get_keyboard(id));
    }
    fn handle_get_touch(&mut self, slf: &Rc<WlSeat>, id: &Rc<WlTouch>) {
        log_send("wl_seat.get_touch", slf.try_send_get_touch(id));
    }
}

struct KeyboardH;
impl WlKeyboardHandler for KeyboardH {
    fn handle_key(
        &mut self,
        slf: &Rc<WlKeyboard>,
        serial: u32,
        time: u32,
        key: u32,
        state: WlKeyboardKeyState,
    ) {
        log_send(
            "wl_keyboard.key",
            slf.try_send_key(serial, time, key, state),
        );
    }
}

// Every other seat is mpv's: swallow its input-device getters so the compositor
// never creates server-side pointer/keyboard/touch for mpv. mpv's VO therefore
// receives no input (the empty input region only blocks pointer; this closes the
// keyboard/touch hole).
struct BlockSeatH;
impl WlSeatHandler for BlockSeatH {
    fn handle_get_pointer(&mut self, _slf: &Rc<WlSeat>, id: &Rc<WlPointer>) {
        id.set_forward_to_server(false);
    }
    fn handle_get_keyboard(&mut self, _slf: &Rc<WlSeat>, id: &Rc<WlKeyboard>) {
        id.set_forward_to_server(false);
    }
    fn handle_get_touch(&mut self, _slf: &Rc<WlSeat>, id: &Rc<WlTouch>) {
        id.set_forward_to_server(false);
    }
}

struct PointerH;
impl WlPointerHandler for PointerH {
    fn handle_button(
        &mut self,
        slf: &Rc<WlPointer>,
        serial: u32,
        time: u32,
        button: u32,
        state: WlPointerButtonState,
    ) {
        log_send(
            "wl_pointer.button",
            slf.try_send_button(serial, time, button, state),
        );
    }
}

struct AppWmBaseH;
impl XdgWmBaseHandler for AppWmBaseH {
    fn handle_get_xdg_surface(
        &mut self,
        slf: &Rc<XdgWmBase>,
        id: &Rc<XdgSurface>,
        surface: &Rc<WlSurface>,
    ) {
        id.set_handler(AppXdgSurfaceH {
            surface: surface.clone(),
        });
        log_send(
            "xdg_wm_base.get_xdg_surface",
            slf.try_send_get_xdg_surface(id, surface),
        );
    }
}

struct AppXdgSurfaceH {
    surface: Rc<WlSurface>,
}
impl XdgSurfaceHandler for AppXdgSurfaceH {
    fn handle_get_toplevel(&mut self, slf: &Rc<XdgSurface>, id: &Rc<XdgToplevel>) {
        tracing::info!(target: "MpvProxy", "get_toplevel: capturing app root surface");
        with_shell(|sh| {
            sh.host_root_surface = Some(self.surface.clone());
            sh.host_root_xdg_surface = Some(slf.clone());
        });
        log_send("xdg_surface.get_toplevel", slf.try_send_get_toplevel(id));
    }
}

struct MpvWmBaseH;
impl XdgWmBaseHandler for MpvWmBaseH {
    fn handle_get_xdg_surface(
        &mut self,
        _slf: &Rc<XdgWmBase>,
        id: &Rc<XdgSurface>,
        surface: &Rc<WlSurface>,
    ) {
        if let Some(sid) = surface.server_id() {
            MPV_VIDEO_SURFACE_ID.store(sid, Ordering::Release);
        }
        tracing::info!(
            target: "MpvProxy",
            "get_xdg_surface: demoting mpv surface server_id={:?}",
            surface.server_id()
        );
        // mpv's surface must stay role-free upstream so we can give it the
        // subsurface role; never forward get_xdg_surface.
        id.set_forward_to_server(false);
        id.set_handler(MpvSurfaceH);
    }
}

struct MpvSurfaceH;
impl XdgSurfaceHandler for MpvSurfaceH {
    fn handle_get_toplevel(&mut self, slf: &Rc<XdgSurface>, id: &Rc<XdgToplevel>) {
        id.set_forward_to_server(false);
        id.set_handler(MpvToplevelH);
        with_shell(|sh| {
            sh.configurator = Some(MpvConfigurator {
                toplevel: id.clone(),
                xdg_surface: slf.clone(),
            });
        });
        // Configure now if the window size is already known (mpv connected after
        // the host was configured — the common case); otherwise the size feed
        // emits once the host publishes. mpv only ever receives real geometry.
        if let Some(size) = window_size() {
            emit_mpv_configure(size);
        }
    }
}

/// mpv blocks waiting for a configure in reply to its state-change request (e.g.
/// unset_maximized from VOCTRL_SET_UNFS_WINDOW_SIZE). We never honor the request
/// — the host owns geometry — but must answer it, or mpv stalls on the dropped
/// message.
fn reassert_mpv_state() {
    if let Some(size) = window_size() {
        emit_mpv_configure(size);
    }
}

struct MpvToplevelH;
impl XdgToplevelHandler for MpvToplevelH {
    fn handle_set_maximized(&mut self, _slf: &Rc<XdgToplevel>) {
        reassert_mpv_state();
    }
    fn handle_unset_maximized(&mut self, _slf: &Rc<XdgToplevel>) {
        reassert_mpv_state();
    }
    fn handle_set_fullscreen(&mut self, _slf: &Rc<XdgToplevel>, _output: Option<&Rc<WlOutput>>) {
        reassert_mpv_state();
    }
    fn handle_unset_fullscreen(&mut self, _slf: &Rc<XdgToplevel>) {
        reassert_mpv_state();
    }
}

fn ensure_root() {
    let (started, display) = with_shell(|sh| (sh.roundtrip_started, sh.display.clone()));
    if started {
        return;
    }
    let Some(display) = display else {
        return;
    };
    with_shell(|sh| sh.roundtrip_started = true);
    let registry = display.create_child::<WlRegistry>();
    registry.set_handler(ProxyRegistryH);
    if let Err(e) = display.try_send_get_registry(&registry) {
        tracing::error!(target: "MpvProxy", "ensure_root get_registry: {}", Report::new(&e));
    }
    let sync = display.create_child::<WlCallback>();
    sync.set_handler(RoundtripCb);
    if let Err(e) = display.try_send_sync(&sync) {
        tracing::error!(target: "MpvProxy", "ensure_root sync: {}", Report::new(&e));
    }
}

fn maybe_build_root() {
    let (ready, spliced, have_host_root) =
        with_shell(|sh| (sh.globals_ready, sh.spliced, sh.host_root_surface.is_some()));
    if !ready || spliced || !have_host_root {
        return;
    }
    if let Some(mpv) = find_mpv_surface() {
        splice_mpv_under_host_root(mpv);
    }
}

fn find_mpv_surface() -> Option<Rc<WlSurface>> {
    let vid = MPV_VIDEO_SURFACE_ID.load(Ordering::Acquire);
    if vid == 0 {
        return None;
    }
    let client = with_shell(|sh| sh.mpv_client.clone())?;
    let mut objs = Vec::new();
    client.objects(&mut objs);
    objs.into_iter().find_map(|o| {
        let s = o.try_downcast::<WlSurface>()?;
        (s.client_id() == Some(vid)).then_some(s)
    })
}

fn splice_mpv_under_host_root(mpv_surface: Rc<WlSurface>) {
    let objs = with_shell(|sh| {
        if sh.spliced {
            return None;
        }
        Some((
            sh.compositor.clone()?,
            sh.subcompositor.clone()?,
            sh.host_root_surface.clone()?,
        ))
    });
    let Some((compositor, subcompositor, host_root)) = objs else {
        return;
    };

    // Gating call: without the subsurface role nothing below applies, so on
    // failure bail without marking spliced — maybe_build_root retries next tick.
    // The subsurface is born synchronized and is never desynced, so mpv's buffer
    // applies atomically with the host root's geometry.
    let sub = match SyncSubsurface::create(&subcompositor, &mpv_surface, &host_root) {
        Ok(s) => s,
        Err(e) => {
            tracing::error!(target: "MpvProxy", "splice get_subsurface: {}", Report::new(&e));
            return;
        }
    };
    if let Err(e) = sub.set_position(0, 0) {
        tracing::error!(target: "MpvProxy", "splice set_position: {}", Report::new(&e));
    }
    // Pin mpv to the bottom of the root's subsurface stack (place_above the
    // parent = lowest sibling position). The CEF overlay is a sibling subsurface
    // on a different client, so creation order can't keep it above the video.
    if let Err(e) = sub.place_above(&host_root) {
        tracing::error!(target: "MpvProxy", "splice place_above: {}", Report::new(&e));
    }

    // mpv's synchronized subsurface only displays when the root commits. Each mpv
    // commit requests a present from the single root-commit owner, so every video
    // frame applies in one transaction with the window's current geometry.
    mpv_surface.set_handler(ChildPresentH);

    let region = compositor.create_child::<WlRegion>();
    if let Err(e) = compositor.try_send_create_region(&region) {
        tracing::error!(target: "MpvProxy", "splice create_region: {}", Report::new(&e));
    }
    if let Err(e) = mpv_surface.try_send_set_input_region(Some(&region)) {
        tracing::error!(target: "MpvProxy", "splice set_input_region: {}", Report::new(&e));
    }
    if let Err(e) = region.try_send_destroy() {
        tracing::error!(target: "MpvProxy", "splice region destroy: {}", Report::new(&e));
    }

    // Adding the subsurface only takes effect on the parent's next commit; force
    // one now so a late splice (after the app already mapped) still becomes
    // visible.
    if let Err(e) = host_root.try_send_commit() {
        tracing::error!(target: "MpvProxy", "splice root commit: {}", Report::new(&e));
    }

    with_shell(|sh| {
        sh.mpv_subsurface = Some(sub);
        sh.spliced = true;
    });
    tracing::info!(target: "MpvProxy", "spliced mpv under host-root surface");
}

struct ProxyRegistryH;
impl WlRegistryHandler for ProxyRegistryH {
    fn handle_global(
        &mut self,
        slf: &Rc<WlRegistry>,
        name: u32,
        interface: ObjectInterface,
        version: u32,
    ) {
        let state = slf.state();
        match interface {
            WlCompositor::INTERFACE => {
                let o = state.create_object::<WlCompositor>(version.min(6));
                log_send("wl_registry.bind", slf.try_send_bind(name, o.clone()));
                with_shell(|sh| sh.compositor = Some(o));
            }
            WlSubcompositor::INTERFACE => {
                let o = state.create_object::<WlSubcompositor>(version.min(1));
                log_send("wl_registry.bind", slf.try_send_bind(name, o.clone()));
                with_shell(|sh| sh.subcompositor = Some(o));
            }
            XdgWmBase::INTERFACE => {
                let o = state.create_object::<XdgWmBase>(version.min(6));
                o.set_handler(ProxyWmBaseH);
                log_send("wl_registry.bind", slf.try_send_bind(name, o.clone()));
                with_shell(|sh| sh.wm_base = Some(o));
            }
            _ => {}
        }
    }
}

struct ProxyWmBaseH;
impl XdgWmBaseHandler for ProxyWmBaseH {
    fn handle_ping(&mut self, slf: &Rc<XdgWmBase>, serial: u32) {
        // The compositor pings our own wm_base; mpv can't pong it, so we must.
        log_send("xdg_wm_base.pong", slf.try_send_pong(serial));
    }
}

struct RoundtripCb;
impl WlCallbackHandler for RoundtripCb {
    fn handle_done(&mut self, _slf: &Rc<WlCallback>, _data: u32) {
        let ok = with_shell(|sh| {
            sh.globals_ready = true;
            sh.compositor.is_some() && sh.subcompositor.is_some() && sh.wm_base.is_some()
        });
        if !ok {
            eprintln!(
                "proxy: missing globals for splice (need compositor, subcompositor, xdg_wm_base)"
            );
        }
    }
}

/// Installed on mpv's video surface: mpv's commit caches its (synchronized)
/// buffer, then a present is requested from the single root-commit owner
/// (`root_window`), which applies it atomically with the window geometry. mpv
/// never commits the root itself — the owner is the sole root committer.
struct ChildPresentH;
impl WlSurfaceHandler for ChildPresentH {
    fn handle_commit(&mut self, slf: &Rc<WlSurface>) {
        log_send("wl_surface.commit", slf.try_send_commit());
        crate::root_window::request_present();
    }
}
