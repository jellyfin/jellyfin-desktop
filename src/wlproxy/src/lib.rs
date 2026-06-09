//! Wayland proxy between mpv and the compositor.
//!
//! mpv connects here instead of the real compositor (via WAYLAND_DISPLAY env).
//! Messages forward in both directions; selected events are intercepted.
//!
//! Interceptions:
//! - `xdg_toplevel.configure` → fan width/height/fullscreen out to a C callback.
//! - `wp_fractional_scale_v1.preferred_scale` → drives scale_120; fires a
//!   separate C callback so the host owns scale state instead of routing
//!   through libmpv's `display-hidpi-scale` property.
//! - `xdg_toplevel.set_fullscreen` / `set_maximized` / unset variants — host
//!   drives these from C via a command queue; the per-client dispatch loop
//!   drains the queue between Wayland event batches.
//!
//! We don't use `SimpleProxy` because it builds each per-client `State` using
//! the current process `WAYLAND_DISPLAY` env to find the upstream compositor —
//! but the caller overrides that env to OUR socket so mpv connects to us. We
//! must capture the original `WAYLAND_DISPLAY` here at `start` (before any
//! override) and pass it explicitly via `with_server_display_name`.
//!
//! The whole crate is gated to Linux: `wl-proxy` is a Wayland-only dependency,
//! and nothing references this crate off-Linux (jfn_rust pulls it in only under
//! its `cfg(target_os = "linux")` deps, and `jfn-wayland` is Linux-only). Off
//! Linux this is an empty rlib, which keeps `cargo --workspace` uniform.
#![cfg(target_os = "linux")]

use parking_lot::Mutex;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::ffi::CString;
use std::os::fd::OwnedFd;
use std::os::raw::{c_char, c_int};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use error_reporter::Report;
use wl_proxy::acceptor::Acceptor;
use wl_proxy::baseline::Baseline;
use wl_proxy::client::{Client, ClientHandler};
use wl_proxy::object::{ConcreteObject, Object, ObjectCoreApi, ObjectRcUtils};
use wl_proxy::protocols::ObjectInterface;
use wl_proxy::protocols::fractional_scale_v1::wp_fractional_scale_manager_v1::{
    WpFractionalScaleManagerV1, WpFractionalScaleManagerV1Handler,
};
use wl_proxy::protocols::fractional_scale_v1::wp_fractional_scale_v1::{
    WpFractionalScaleV1, WpFractionalScaleV1Handler,
};
use wl_proxy::protocols::org_kde_kwin_server_decoration_palette_v1::org_kde_kwin_server_decoration_palette::OrgKdeKwinServerDecorationPalette;
use wl_proxy::protocols::org_kde_kwin_server_decoration_palette_v1::org_kde_kwin_server_decoration_palette_manager::OrgKdeKwinServerDecorationPaletteManager;
use wl_proxy::protocols::single_pixel_buffer_v1::wp_single_pixel_buffer_manager_v1::WpSinglePixelBufferManagerV1;
use wl_proxy::protocols::viewporter::wp_viewport::{WpViewport, WpViewportHandler};
use wl_proxy::protocols::viewporter::wp_viewporter::{WpViewporter, WpViewporterHandler};
use wl_proxy::protocols::wayland::wl_buffer::WlBuffer;
use wl_proxy::protocols::wayland::wl_callback::{WlCallback, WlCallbackHandler};
use wl_proxy::protocols::wayland::wl_compositor::WlCompositor;
use wl_proxy::protocols::wayland::wl_display::{WlDisplay, WlDisplayHandler};
use wl_proxy::protocols::wayland::wl_keyboard::{WlKeyboard, WlKeyboardHandler, WlKeyboardKeyState};
use wl_proxy::protocols::wayland::wl_pointer::{WlPointer, WlPointerButtonState, WlPointerHandler};
use wl_proxy::protocols::wayland::wl_region::WlRegion;
use wl_proxy::protocols::wayland::wl_registry::{WlRegistry, WlRegistryHandler};
use wl_proxy::protocols::wayland::wl_seat::{WlSeat, WlSeatHandler};
use wl_proxy::protocols::wayland::wl_subcompositor::WlSubcompositor;
use wl_proxy::protocols::wayland::wl_subsurface::WlSubsurface;
use wl_proxy::protocols::wayland::wl_surface::WlSurface;
use wl_proxy::protocols::wayland::wl_touch::WlTouch;
use wl_proxy::protocols::xdg_decoration_unstable_v1::zxdg_decoration_manager_v1::ZxdgDecorationManagerV1;
use wl_proxy::protocols::xdg_decoration_unstable_v1::zxdg_toplevel_decoration_v1::{
    ZxdgToplevelDecorationV1, ZxdgToplevelDecorationV1Mode,
};
use wl_proxy::protocols::xdg_shell::xdg_popup::{XdgPopup, XdgPopupHandler};
use wl_proxy::protocols::xdg_shell::xdg_positioner::{
    XdgPositioner, XdgPositionerAnchor, XdgPositionerConstraintAdjustment, XdgPositionerGravity,
};
use wl_proxy::protocols::xdg_shell::xdg_surface::{XdgSurface, XdgSurfaceHandler};
use wl_proxy::protocols::xdg_shell::xdg_toplevel::{
    XdgToplevel, XdgToplevelHandler, XdgToplevelResizeEdge, XdgToplevelState,
};
use wl_proxy::protocols::xdg_shell::xdg_wm_base::{XdgWmBase, XdgWmBaseHandler};
use wl_proxy::state::State;

pub struct Proxy {
    display_name: CString,
    _thread: thread::JoinHandle<()>,
}

type ConfigureCb = extern "C" fn(c_int, c_int, c_int, c_int);
type ScaleCb = extern "C" fn(c_int);
type SuspendedCb = extern "C" fn(c_int);
type PopupReadyCb = extern "C" fn();
type PopupDoneCb = extern "C" fn();
type CloseCb = extern "C" fn();

// Single proxy per process — callbacks are global. Fire from the per-client
// proxy thread; the C side guards against thread-safety with its own mutex.
static CONFIGURE_CB: Mutex<Option<ConfigureCb>> = Mutex::new(None);
static SCALE_CB: Mutex<Option<ScaleCb>> = Mutex::new(None);
static SUSPENDED_CB: Mutex<Option<SuspendedCb>> = Mutex::new(None);
static POPUP_READY_CB: Mutex<Option<PopupReadyCb>> = Mutex::new(None);
static POPUP_DONE_CB: Mutex<Option<PopupDoneCb>> = Mutex::new(None);
static CLOSE_CB: Mutex<Option<CloseCb>> = Mutex::new(None);

static POPUP_SURFACE_ID: AtomicU32 = AtomicU32::new(0);
// Last reported suspended state to suppress no-op edges (the compositor
// repeats the state on every configure).
static LAST_SUSPENDED: Mutex<c_int> = Mutex::new(0);

enum HostCommand {
    SetFullscreen(bool),
    SetMaximized(bool),
    SetMinimized,
    Move,
    Resize(u32),
    ShowPopup { x: i32, y: i32, w: i32, h: i32 },
    RepositionPopup { x: i32, y: i32, w: i32, h: i32 },
    HidePopup,
    SetBackground,
    SetTitlebarPalette,
}

static COMMANDS: Mutex<VecDeque<HostCommand>> = Mutex::new(VecDeque::new());

const DECO_CSD: u32 = 1;
const DECO_SERVER_THEMED: u32 = 3;

static DECORATION_MODE: AtomicU32 = AtomicU32::new(0);

static BACKGROUND_RGBA: AtomicU32 = AtomicU32::new(0);

static PENDING_PALETTE: Mutex<Option<CString>> = Mutex::new(None);

static HOST_SURFACE_ID: AtomicU32 = AtomicU32::new(0);

static HOST_INPUT_SEAT_ID: AtomicU32 = AtomicU32::new(0);

// Connect-order index per in-process client must stay in lockstep with the
// binary's `wl_display_connect` interposer counter, so an index maps to a
// captured `wl_display*`.
static SAME_PROC_SEQ: AtomicU32 = AtomicU32::new(0);

static VO_CONNECTION_INDEX: AtomicI32 = AtomicI32::new(-1);

static INITIAL_W: AtomicI32 = AtomicI32::new(1280);
static INITIAL_H: AtomicI32 = AtomicI32::new(720);
static INITIAL_MAXIMIZED: AtomicBool = AtomicBool::new(false);

pub extern "C" fn jfn_wlproxy_vo_connection_index() -> c_int {
    VO_CONNECTION_INDEX.load(Ordering::Acquire)
}

/// Must be called before mpv connects: root construction reads this once.
pub fn jfn_wlproxy_set_initial_size(w: c_int, h: c_int) {
    if w > 0 && h > 0 {
        INITIAL_W.store(w, Ordering::Release);
        INITIAL_H.store(h, Ordering::Release);
    }
}

/// Must be called before mpv connects: requested on the root toplevel as part
/// of its initial map, so a restored-maximized window comes up maximized with
/// no unmaximized flash.
pub fn jfn_wlproxy_set_initial_maximized(maximized: bool) {
    INITIAL_MAXIMIZED.store(maximized, Ordering::Release);
}

fn initial_size() -> (c_int, c_int) {
    (
        INITIAL_W.load(Ordering::Acquire),
        INITIAL_H.load(Ordering::Acquire),
    )
}

thread_local! {
    // Set while the proxy opens its OWN upstream connection. The interposer
    // checks this to skip recording the proxy's upstream displays, else the
    // capture index would misalign with `SAME_PROC_SEQ`.
    static IN_UPSTREAM_CONNECT: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

pub fn jfn_wlproxy_in_upstream_connect() -> bool {
    IN_UPSTREAM_CONNECT.with(std::cell::Cell::get)
}

struct UpstreamConnectGuard;
impl UpstreamConnectGuard {
    fn enter() -> Self {
        IN_UPSTREAM_CONNECT.with(|c| c.set(true));
        Self
    }
}
impl Drop for UpstreamConnectGuard {
    fn drop(&mut self) {
        IN_UPSTREAM_CONNECT.with(|c| c.set(false));
    }
}

fn socket_peer_pid(fd: std::os::fd::RawFd) -> Option<i32> {
    let mut cred = std::mem::MaybeUninit::<libc::ucred>::uninit();
    let mut len = std::mem::size_of::<libc::ucred>() as libc::socklen_t;
    let rc = unsafe {
        libc::getsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            cred.as_mut_ptr().cast(),
            &mut len,
        )
    };
    (rc == 0).then(|| unsafe { cred.assume_init() }.pid)
}

pub fn jfn_wlproxy_set_host_surface(id: u32) {
    HOST_SURFACE_ID.store(id, Ordering::Release);
}

pub extern "C" fn jfn_wlproxy_set_decoration_mode(mode: c_int) {
    DECORATION_MODE.store(mode as u32, Ordering::Release);
}

pub extern "C" fn jfn_wlproxy_set_background_color(r: u8, g: u8, b: u8) {
    let rgb = (u32::from(r) << 16) | (u32::from(g) << 8) | u32::from(b);
    BACKGROUND_RGBA.store(rgb, Ordering::Release);
    COMMANDS.lock().push_back(HostCommand::SetBackground);
}

/// # Safety
/// `path` must be null or a valid NUL-terminated C string.
pub unsafe extern "C" fn jfn_wlproxy_set_titlebar_palette(path: *const c_char) {
    if path.is_null() {
        return;
    }
    let owned = unsafe { std::ffi::CStr::from_ptr(path) }.to_owned();
    *PENDING_PALETTE.lock() = Some(owned);
    COMMANDS.lock().push_back(HostCommand::SetTitlebarPalette);
}

pub fn jfn_wlproxy_set_input_seat(id: u32) {
    HOST_INPUT_SEAT_ID.store(id, Ordering::Release);
}

pub fn jfn_wlproxy_set_popup_surface(id: u32) {
    POPUP_SURFACE_ID.store(id, Ordering::Release);
}

thread_local! {
    // xdg_toplevel.move/resize requires a serial from THIS connection (the
    // toplevel's), not the mpv-side input subsystem's serial namespace.
    static SEAT: RefCell<Option<Rc<WlSeat>>> = const { RefCell::new(None) };
    static LAST_SERIAL: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };

    static SHELL: RefCell<Shell> = const { RefCell::new(Shell::new()) };
}

struct Shell {
    display: Option<Rc<WlDisplay>>,
    client: Option<Rc<Client>>,
    compositor: Option<Rc<WlCompositor>>,
    subcompositor: Option<Rc<WlSubcompositor>>,
    wm_base: Option<Rc<XdgWmBase>>,
    spbm: Option<Rc<WpSinglePixelBufferManagerV1>>,
    viewporter: Option<Rc<WpViewporter>>,
    decoration_manager: Option<Rc<ZxdgDecorationManagerV1>>,
    palette_manager: Option<Rc<OrgKdeKwinServerDecorationPaletteManager>>,
    globals_ready: bool,
    roundtrip_started: bool,
    root_surface: Option<Rc<WlSurface>>,
    root_xdg_surface: Option<Rc<XdgSurface>>,
    root_toplevel: Option<Rc<XdgToplevel>>,
    root_decoration: Option<Rc<ZxdgToplevelDecorationV1>>,
    root_palette: Option<Rc<OrgKdeKwinServerDecorationPalette>>,
    root_buffer: Option<Rc<WlBuffer>>,
    root_viewport: Option<Rc<WpViewport>>,
    root_mapped: bool,
    mpv_surface: Option<Rc<WlSurface>>,
    mpv_xdg_surface: Option<Rc<XdgSurface>>,
    mpv_toplevel: Option<Rc<XdgToplevel>>,
    mpv_subsurface: Option<Rc<WlSubsurface>>,
    demote_pending: bool,
    host_adopted: bool,
    host_surface: Option<Rc<WlSurface>>,
    host_subsurface: Option<Rc<WlSubsurface>>,
    host_viewport: Option<Rc<WpViewport>>,
    popup_surface: Option<Rc<WlSurface>>,
    popup_xdg_surface: Option<Rc<XdgSurface>>,
    popup: Option<Rc<XdgPopup>>,
    cur_w: i32,
    cur_h: i32,
    cur_states: Vec<u8>,
    serial: u32,
    same_proc_index: Option<u32>,
}

impl Shell {
    const fn new() -> Self {
        Self {
            display: None,
            client: None,
            compositor: None,
            subcompositor: None,
            wm_base: None,
            spbm: None,
            viewporter: None,
            decoration_manager: None,
            palette_manager: None,
            globals_ready: false,
            roundtrip_started: false,
            root_surface: None,
            root_xdg_surface: None,
            root_toplevel: None,
            root_decoration: None,
            root_palette: None,
            root_buffer: None,
            root_viewport: None,
            root_mapped: false,
            mpv_surface: None,
            mpv_xdg_surface: None,
            mpv_toplevel: None,
            mpv_subsurface: None,
            demote_pending: false,
            host_adopted: false,
            host_surface: None,
            host_subsurface: None,
            host_viewport: None,
            popup_surface: None,
            popup_xdg_surface: None,
            popup: None,
            cur_w: 0,
            cur_h: 0,
            cur_states: Vec::new(),
            serial: 0,
            same_proc_index: None,
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

// Pending state for configure synthesis. We own the scale tracking via
// wp_fractional_scale_v1.preferred_scale and convert the logical width/height
// from xdg_toplevel.configure into physical pixels before invoking the
// callback. Either event (configure or preferred_scale) can fire first; the
// last-known values from both sides are recombined on every change.
//
// Scale wire encoding: numerator over WAYLAND_SCALE_FACTOR = 120. Default 120
// means scale = 1.0 (matches behavior on non-fractional compositors before any
// preferred_scale event has been seen).
struct PendingConfigure {
    have_configure: bool,
    logical_w: i32,
    logical_h: i32,
    fullscreen: c_int,
    maximized: c_int,
    scale_120: u32,
}

static PENDING: Mutex<PendingConfigure> = Mutex::new(PendingConfigure {
    have_configure: false,
    logical_w: 0,
    logical_h: 0,
    fullscreen: 0,
    maximized: 0,
    scale_120: 120,
});

fn fire_suspended(suspended: c_int) {
    {
        let mut last = LAST_SUSPENDED.lock();
        if *last == suspended {
            return;
        }
        *last = suspended;
    }
    if let Some(cb) = *SUSPENDED_CB.lock() {
        cb(suspended);
    }
}

fn fire_configure() {
    let p = PENDING.lock();
    if !p.have_configure {
        return;
    }
    // Round half-up: (logical * scale_120 + WAYLAND_SCALE_FACTOR/2) / WAYLAND_SCALE_FACTOR.
    let pw = ((p.logical_w as i64 * p.scale_120 as i64 + 60) / 120) as c_int;
    let ph = ((p.logical_h as i64 * p.scale_120 as i64 + 60) / 120) as c_int;
    let fs = p.fullscreen;
    let max = p.maximized;
    drop(p);
    if let Some(cb) = *CONFIGURE_CB.lock() {
        cb(pw, ph, fs, max);
    }
}

/// Start the proxy. Spawns a listener thread that owns the `Acceptor` (which
/// is `!Send` because it holds `Rc` internally, so it must be constructed
/// inside the thread). The thread hands the listening socket name back via a
/// channel before entering its blocking accept loop.
///
/// Returns null on failure.
pub fn jfn_wlproxy_start() -> *mut Proxy {
    // Capture upstream BEFORE the caller overrides WAYLAND_DISPLAY. Per-client
    // States need this so they don't connect to our own socket.
    let upstream = std::env::var("WAYLAND_DISPLAY").ok();

    let (tx, rx) = mpsc::sync_channel::<Result<CString, String>>(1);
    let thread = match thread::Builder::new()
        .name("wlproxy".into())
        .spawn(move || run_listener(tx, upstream))
    {
        Ok(h) => h,
        Err(e) => {
            eprintln!("wlproxy: thread spawn failed: {e}");
            return std::ptr::null_mut();
        }
    };
    let display_name = match rx.recv() {
        Ok(Ok(n)) => n,
        Ok(Err(msg)) => {
            eprintln!("wlproxy: {msg}");
            return std::ptr::null_mut();
        }
        Err(_) => {
            eprintln!("wlproxy: listener thread exited before sending display name");
            return std::ptr::null_mut();
        }
    };
    Box::into_raw(Box::new(Proxy {
        display_name,
        _thread: thread,
    }))
}

/// Returns the WAYLAND_DISPLAY value clients should connect to (e.g. "wayland-1").
/// Returns null if `p` is null. Pointer is valid until `jfn_wlproxy_stop`.
///
/// # Safety
/// `p` must be null or a pointer previously returned by `jfn_wlproxy_start`
/// that has not yet been passed to `jfn_wlproxy_stop`.
pub unsafe fn jfn_wlproxy_display_name(p: *const Proxy) -> *const c_char {
    if p.is_null() {
        return std::ptr::null();
    }
    unsafe { (*p).display_name.as_ptr() }
}

/// Drop the proxy handle. The listener thread is detached; OS cleans up on
/// process exit. Safe to call with null.
///
/// # Safety
/// `p` must be null or a pointer previously returned by `jfn_wlproxy_start`.
/// Each non-null pointer may only be passed here once.
pub unsafe fn jfn_wlproxy_stop(p: *mut Proxy) {
    if p.is_null() {
        return;
    }
    unsafe { drop(Box::from_raw(p)) };
}

/// Register the xdg_toplevel.configure interception callback.
///
/// Fires from the proxy's per-client thread whenever the compositor sends an
/// `xdg_toplevel.configure` event. Arguments are
/// `(width, height, fullscreen, maximized)` — fullscreen/maximized are 1 if the
/// configure's states[] array contains `XDG_TOPLEVEL_STATE_FULLSCREEN` /
/// `_MAXIMIZED`, 0 otherwise. width/height are physical pixels (scaled by the
/// current `scale_120 / 120` factor).
///
/// The event still forwards to mpv after the callback runs.
pub fn jfn_wlproxy_set_configure_callback(cb: ConfigureCb) {
    *CONFIGURE_CB.lock() = Some(cb);
}

/// Register the wp_fractional_scale_v1.preferred_scale callback.
///
/// Argument is the scale numerator over `WAYLAND_SCALE_FACTOR=120` (so 120 =
/// 1.0x, 180 = 1.5x, 240 = 2.0x). Fires once whenever the compositor sends a
/// new preferred scale for the toplevel's surface.
pub fn jfn_wlproxy_set_scale_callback(cb: ScaleCb) {
    *SCALE_CB.lock() = Some(cb);
}

/// Register the xdg_toplevel suspended-state callback.
///
/// Fires once on each transition into or out of `XDG_TOPLEVEL_STATE_SUSPENDED`
/// (xdg-shell v6+). Argument is 1 when suspended (compositor signals updates
/// have no user-visible effect, e.g. desktop switched, minimised on KDE),
/// 0 when restored. Repeats are suppressed.
pub fn jfn_wlproxy_set_suspended_callback(cb: SuspendedCb) {
    *SUSPENDED_CB.lock() = Some(cb);
}

/// Clear all host callbacks at shutdown. The dispatch thread must stop
/// re-entering host code, which takes a `WlState` lock a terminating CEF paint
/// thread may have orphaned; otherwise mpv's VO teardown roundtrip deadlocks.
pub fn jfn_wlproxy_clear_callbacks() {
    *CONFIGURE_CB.lock() = None;
    *SCALE_CB.lock() = None;
    *SUSPENDED_CB.lock() = None;
    *POPUP_READY_CB.lock() = None;
    *POPUP_DONE_CB.lock() = None;
    *CLOSE_CB.lock() = None;
}

pub fn jfn_wlproxy_set_close_callback(cb: CloseCb) {
    *CLOSE_CB.lock() = Some(cb);
}

pub fn jfn_wlproxy_set_popup_ready_callback(cb: PopupReadyCb) {
    *POPUP_READY_CB.lock() = Some(cb);
}

pub fn jfn_wlproxy_set_popup_done_callback(cb: PopupDoneCb) {
    *POPUP_DONE_CB.lock() = Some(cb);
}

pub fn jfn_wlproxy_show_popup(x: c_int, y: c_int, w: c_int, h: c_int) {
    COMMANDS
        .lock()
        .push_back(HostCommand::ShowPopup { x, y, w, h });
}

pub fn jfn_wlproxy_reposition_popup(x: c_int, y: c_int, w: c_int, h: c_int) {
    COMMANDS
        .lock()
        .push_back(HostCommand::RepositionPopup { x, y, w, h });
}

pub fn jfn_wlproxy_hide_popup() {
    COMMANDS.lock().push_back(HostCommand::HidePopup);
}

/// Queue an xdg_toplevel.set_fullscreen / unset_fullscreen request. Applied
/// from the proxy's per-client thread on its next dispatch iteration.
pub extern "C" fn jfn_wlproxy_set_fullscreen(enable: c_int) {
    COMMANDS
        .lock()
        .push_back(HostCommand::SetFullscreen(enable != 0));
}

/// Queue an xdg_toplevel.set_maximized / unset_maximized request. Applied
/// from the proxy's per-client thread on its next dispatch iteration.
pub fn jfn_wlproxy_set_maximized(enable: c_int) {
    COMMANDS
        .lock()
        .push_back(HostCommand::SetMaximized(enable != 0));
}

/// Queue an xdg_toplevel.set_minimized request.
pub fn jfn_wlproxy_set_minimized() {
    COMMANDS.lock().push_back(HostCommand::SetMinimized);
}

/// Queue an interactive xdg_toplevel.move. Uses the most recent pointer-button
/// serial seen on this connection. Must be called in response to a button press
/// (the compositor takes over the drag grab).
pub fn jfn_wlproxy_window_move() {
    COMMANDS.lock().push_back(HostCommand::Move);
}

/// Queue an interactive xdg_toplevel.resize. `edge` is an xdg_toplevel
/// resize-edge value (1=top, 2=bottom, 4=left, 8=right, and their corner ORs).
/// Like move, uses the most recent pointer-button serial.
pub fn jfn_wlproxy_window_resize(edge: c_int) {
    COMMANDS.lock().push_back(HostCommand::Resize(edge as u32));
}

fn run_listener(tx: mpsc::SyncSender<Result<CString, String>>, upstream: Option<String>) {
    let acceptor = match Acceptor::new(1000, false) {
        Ok(a) => a,
        Err(e) => {
            let _ = tx.send(Err(format!("Acceptor::new: {}", Report::new(e))));
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

    let upstream = upstream.as_deref();
    loop {
        let socket = match acceptor.accept() {
            Ok(Some(s)) => s,
            Ok(None) => continue,
            Err(e) => {
                eprintln!("wlproxy: accept failed: {}", Report::new(e));
                return;
            }
        };
        let upstream_owned = upstream.map(str::to_owned);
        let _ = thread::Builder::new()
            .name("wlproxy-client".into())
            .spawn(move || run_client(socket, upstream_owned));
    }
}

fn run_client(socket: OwnedFd, upstream: Option<String>) {
    use std::os::fd::AsRawFd;
    // Only same-process clients (mpv's VO) get a connect-order index matching
    // the interposer's capture order; out-of-process clients (CEF) do not.
    let same_proc_index = (socket_peer_pid(socket.as_raw_fd()) == Some(std::process::id() as i32))
        .then(|| SAME_PROC_SEQ.fetch_add(1, Ordering::AcqRel));

    let mut builder = State::builder(Baseline::ALL_OF_THEM).with_log_prefix("jfn");
    if let Some(name) = &upstream {
        builder = builder.with_server_display_name(name);
    }
    let state = {
        // build() opens the upstream connection — guard it so the interposer
        // skips that display (not a VO candidate).
        let _guard = UpstreamConnectGuard::enter();
        match builder.build() {
            Ok(s) => s,
            Err(e) => {
                eprintln!("wlproxy: State::build: {}", Report::new(e));
                return;
            }
        }
    };
    let client = match state.add_client(&Rc::new(socket)) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("wlproxy: add_client: {}", Report::new(e));
            return;
        }
    };
    client.set_handler(NoopClient);
    client.display().set_handler(DisplayH);
    with_shell(|sh| {
        sh.client = Some(client.clone());
        sh.same_proc_index = same_proc_index;
    });

    // Dispatch with a short timeout so the loop also services the host
    // command queue (set_fullscreen / set_maximized) within ~16ms even when
    // no Wayland events are arriving. Real events return immediately from
    // poll — the timeout only fires during idle periods.
    while state.is_not_destroyed() {
        match state.dispatch(Some(Duration::from_millis(16))) {
            Ok(_) => {}
            Err(e) => {
                eprintln!("wlproxy: dispatch: {}", Report::new(e));
                return;
            }
        }
        drain_host_commands();
        maybe_build_root();
    }
}

fn drain_host_commands() {
    // Only the client thread that built the root drains — COMMANDS is
    // process-global but the root lives in this thread's SHELL, so a thread
    // without a root must NOT drain (else it pops and drops, racing the owner).
    let Some(tl) = with_shell(|sh| sh.root_toplevel.clone()) else {
        return;
    };
    let cmds: Vec<HostCommand> = COMMANDS.lock().drain(..).collect();
    for cmd in cmds {
        match cmd {
            HostCommand::SetFullscreen(true) => tl.send_set_fullscreen(None),
            HostCommand::SetFullscreen(false) => tl.send_unset_fullscreen(),
            HostCommand::SetMaximized(true) => tl.send_set_maximized(),
            HostCommand::SetMaximized(false) => tl.send_unset_maximized(),
            HostCommand::SetMinimized => tl.send_set_minimized(),
            HostCommand::Move => SEAT.with(|s| {
                if let Some(seat) = s.borrow().as_ref() {
                    tl.send_move(seat, LAST_SERIAL.with(|c| c.get()));
                }
            }),
            HostCommand::Resize(edge) => SEAT.with(|s| {
                if let Some(seat) = s.borrow().as_ref() {
                    tl.send_resize(
                        seat,
                        LAST_SERIAL.with(|c| c.get()),
                        XdgToplevelResizeEdge(edge),
                    );
                }
            }),
            HostCommand::ShowPopup { x, y, w, h } => create_popup(x, y, w, h),
            HostCommand::RepositionPopup { x, y, w, h } => reposition_popup(x, y, w, h),
            HostCommand::HidePopup => destroy_popup(),
            HostCommand::SetBackground => refill_root_background(),
            HostCommand::SetTitlebarPalette => apply_titlebar_palette(),
        }
    }
}

fn fire_popup_ready() {
    if let Some(cb) = *POPUP_READY_CB.lock() {
        cb();
    }
}

fn fire_popup_done() {
    if let Some(cb) = *POPUP_DONE_CB.lock() {
        cb();
    }
}

fn find_popup_surface() -> Option<Rc<WlSurface>> {
    let id = POPUP_SURFACE_ID.load(Ordering::Acquire);
    if id == 0 {
        return None;
    }
    let client = with_shell(|sh| sh.client.clone())?;
    let mut objs = Vec::new();
    client.objects(&mut objs);
    objs.into_iter().find_map(|o| {
        let s = o.try_downcast::<WlSurface>()?;
        (s.client_id() == Some(id)).then_some(s)
    })
}

fn build_menu_positioner(
    wm_base: &Rc<XdgWmBase>,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
) -> Rc<XdgPositioner> {
    let positioner = wm_base.create_child::<XdgPositioner>();
    wm_base.send_create_positioner(&positioner);
    positioner.send_set_size(w.max(1), h.max(1));
    positioner.send_set_anchor_rect(x, y, 1, 1);
    positioner.send_set_anchor(XdgPositionerAnchor::TOP_LEFT);
    positioner.send_set_gravity(XdgPositionerGravity::BOTTOM_RIGHT);
    positioner.send_set_constraint_adjustment(XdgPositionerConstraintAdjustment(
        XdgPositionerConstraintAdjustment::FLIP_X.0
            | XdgPositionerConstraintAdjustment::FLIP_Y.0
            | XdgPositionerConstraintAdjustment::SLIDE_X.0
            | XdgPositionerConstraintAdjustment::SLIDE_Y.0,
    ));
    positioner
}

// reposition keeps the popup's grab; destroying and recreating at the new size
// would drop it. Requires the popup to already be mapped.
fn reposition_popup(x: i32, y: i32, w: i32, h: i32) {
    let Some((wm_base, popup)) = with_shell(|sh| Some((sh.wm_base.clone()?, sh.popup.clone()?)))
    else {
        return;
    };
    let positioner = build_menu_positioner(&wm_base, x, y, w, h);
    popup.send_reposition(&positioner, 0);
    positioner.send_destroy();
}

fn create_popup(x: i32, y: i32, w: i32, h: i32) {
    if with_shell(|sh| sh.popup.is_some()) {
        destroy_popup();
    }
    let Some(menu_surface) = find_popup_surface() else {
        eprintln!("wlproxy: show_popup but no popup surface registered");
        return;
    };
    let Some((wm_base, root_xdg)) =
        with_shell(|sh| Some((sh.wm_base.clone()?, sh.root_xdg_surface.clone()?)))
    else {
        return;
    };
    let positioner = build_menu_positioner(&wm_base, x, y, w, h);

    let xdg = wm_base.create_child::<XdgSurface>();
    xdg.set_handler(PopupXdgSurfaceH);
    wm_base.send_get_xdg_surface(&xdg, &menu_surface);

    let popup = xdg.create_child::<XdgPopup>();
    popup.set_handler(PopupH);
    xdg.send_get_popup(&popup, Some(&root_xdg), &positioner);
    positioner.send_destroy();

    SEAT.with(|s| {
        if let Some(seat) = s.borrow().as_ref() {
            popup.send_grab(seat, LAST_SERIAL.with(|c| c.get()));
        }
    });

    // Roleless commit (no buffer) to elicit the first xdg_surface.configure.
    menu_surface.send_commit();

    with_shell(|sh| {
        sh.popup_surface = Some(menu_surface);
        sh.popup_xdg_surface = Some(xdg);
        sh.popup = Some(popup);
    });
}

// Tear down only the proxy-owned role objects; the host owns and destroys the
// underlying `wl_surface` itself.
fn destroy_popup() {
    let (popup, xdg) = with_shell(|sh| (sh.popup.take(), sh.popup_xdg_surface.take()));
    with_shell(|sh| sh.popup_surface = None);
    // Leave POPUP_SURFACE_ID set: the menu wl_surface is persistent, and
    // create_popup destroys the prior role via this fn before re-finding the
    // surface — zeroing here would make that lookup fail on replacement.
    if let Some(p) = popup {
        p.send_destroy();
    }
    if let Some(x) = xdg {
        x.send_destroy();
    }
}

struct PopupXdgSurfaceH;
impl XdgSurfaceHandler for PopupXdgSurfaceH {
    fn handle_configure(&mut self, slf: &Rc<XdgSurface>, serial: u32) {
        slf.send_ack_configure(serial);
        fire_popup_ready();
    }
}

struct PopupH;
impl XdgPopupHandler for PopupH {
    fn handle_popup_done(&mut self, _slf: &Rc<XdgPopup>) {
        fire_popup_done();
        destroy_popup();
    }
}

struct NoopClient;
impl ClientHandler for NoopClient {
    fn disconnected(self: Box<Self>) {}
}

struct DisplayH;
impl WlDisplayHandler for DisplayH {
    fn handle_get_registry(&mut self, slf: &Rc<WlDisplay>, registry: &Rc<WlRegistry>) {
        with_shell(|sh| {
            if sh.display.is_none() {
                sh.display = Some(slf.clone());
            }
        });
        registry.set_handler(RegistryH);
        slf.send_get_registry(registry);
    }
}

struct RegistryH;
impl WlRegistryHandler for RegistryH {
    fn handle_bind(&mut self, slf: &Rc<WlRegistry>, name: u32, id: Rc<dyn Object>) {
        match id.interface() {
            XdgWmBase::INTERFACE => {
                id.downcast::<XdgWmBase>().set_handler(WmBaseH);
            }
            WpFractionalScaleManagerV1::INTERFACE => {
                id.downcast::<WpFractionalScaleManagerV1>()
                    .set_handler(FracScaleMgrH);
            }
            WlSeat::INTERFACE => {
                let seat = id.downcast::<WlSeat>();
                if seat.client_id() == Some(HOST_INPUT_SEAT_ID.load(Ordering::Acquire)) {
                    seat.set_handler(ForwardSeatH);
                    SEAT.with(|s| *s.borrow_mut() = Some(seat.clone()));
                } else {
                    seat.set_handler(BlockSeatH);
                }
            }
            WpViewporter::INTERFACE => {
                id.downcast::<WpViewporter>().set_handler(ClientViewporterH);
            }
            _ => {}
        }
        slf.send_bind(name, id);
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
        slf.send_get_viewport(id, surface);
    }
}

struct ClientViewportH;
impl WpViewportHandler for ClientViewportH {
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
        slf.send_set_destination(width, height);
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
        slf.send_get_fractional_scale(id, surface);
    }
}

struct FracScaleH;
impl WpFractionalScaleV1Handler for FracScaleH {
    fn handle_preferred_scale(&mut self, slf: &Rc<WpFractionalScaleV1>, scale: u32) {
        PENDING.lock().scale_120 = scale;
        if let Some(cb) = *SCALE_CB.lock() {
            cb(scale as c_int);
        }
        fire_configure();
        slf.send_preferred_scale(scale);
    }
}

struct ForwardSeatH;
impl WlSeatHandler for ForwardSeatH {
    fn handle_get_pointer(&mut self, slf: &Rc<WlSeat>, id: &Rc<WlPointer>) {
        id.set_handler(PointerH);
        slf.send_get_pointer(id);
    }
    fn handle_get_keyboard(&mut self, slf: &Rc<WlSeat>, id: &Rc<WlKeyboard>) {
        id.set_handler(KeyboardH);
        slf.send_get_keyboard(id);
    }
    fn handle_get_touch(&mut self, slf: &Rc<WlSeat>, id: &Rc<WlTouch>) {
        slf.send_get_touch(id);
    }
}

// Records the serial of key presses so a keyboard-triggered menu grabs with a
// valid serial, mirroring PointerH for buttons.
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
        if state == WlKeyboardKeyState::PRESSED {
            LAST_SERIAL.with(|c| c.set(serial));
        }
        slf.send_key(serial, time, key, state);
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
        if state == WlPointerButtonState::PRESSED {
            LAST_SERIAL.with(|c| c.set(serial));
        }
        slf.send_button(serial, time, button, state);
    }
}

const STATE_SUSPENDED: u32 = 9;

struct WmBaseH;
impl XdgWmBaseHandler for WmBaseH {
    fn handle_get_xdg_surface(
        &mut self,
        _slf: &Rc<XdgWmBase>,
        id: &Rc<XdgSurface>,
        surface: &Rc<WlSurface>,
    ) {
        // mpv's surface must stay role-free at the compositor so we can give it
        // the subsurface role; never forward get_xdg_surface.
        id.set_forward_to_server(false);
        id.set_handler(MpvSurfaceH);
        with_shell(|sh| {
            sh.mpv_surface = Some(surface.clone());
            sh.mpv_xdg_surface = Some(id.clone());
        });
    }
}

struct MpvSurfaceH;
impl XdgSurfaceHandler for MpvSurfaceH {
    fn handle_get_toplevel(&mut self, _slf: &Rc<XdgSurface>, id: &Rc<XdgToplevel>) {
        id.set_forward_to_server(false);
        id.set_handler(MpvToplevelH);
        with_shell(|sh| {
            sh.mpv_toplevel = Some(id.clone());
            sh.demote_pending = true;
            if sh.cur_w == 0 || sh.cur_h == 0 {
                let (w, h) = initial_size();
                sh.cur_w = w;
                sh.cur_h = h;
            }
            if let Some(idx) = sh.same_proc_index {
                VO_CONNECTION_INDEX.store(idx as i32, Ordering::Release);
            }
        });
        // Hand mpv an immediate initial configure so its geometry is non-zero
        // before it sizes its viewports. Building the real root + its compositor
        // configure is async (registry roundtrip), and mpv's preferred_scale /
        // viewport sizing fires first — a 0 geometry there yields an invalid
        // wp_viewport.set_destination(0,0). The root configure refreshes this.
        let (w, h) = with_shell(|sh| (sh.cur_w, sh.cur_h));
        synth_mpv_configure(w, h, &[]);
        ensure_root();
    }
}

struct MpvToplevelH;
impl XdgToplevelHandler for MpvToplevelH {}

fn ensure_root() {
    let (started, display) = with_shell(|sh| (sh.roundtrip_started, sh.display.clone()));
    if started {
        return;
    }
    let Some(display) = display else {
        eprintln!("wlproxy: no display captured; cannot build root");
        return;
    };
    with_shell(|sh| sh.roundtrip_started = true);
    let registry = display.create_child::<WlRegistry>();
    registry.set_handler(ProxyRegistryH);
    display.send_get_registry(&registry);
    let sync = display.create_child::<WlCallback>();
    sync.set_handler(RoundtripCb);
    display.send_sync(&sync);
}

fn find_host_surface() -> Option<Rc<WlSurface>> {
    let host_id = HOST_SURFACE_ID.load(Ordering::Acquire);
    if host_id == 0 {
        return None;
    }
    let client = with_shell(|sh| sh.client.clone())?;
    let mut objs = Vec::new();
    client.objects(&mut objs);
    objs.into_iter().find_map(|o| {
        let s = o.try_downcast::<WlSurface>()?;
        (s.client_id() == Some(host_id)).then_some(s)
    })
}

fn maybe_build_root() {
    let (ready, pending, built, adopted) = with_shell(|sh| {
        (
            sh.globals_ready,
            sh.demote_pending,
            sh.root_surface.is_some(),
            sh.host_adopted,
        )
    });
    if !ready || !pending {
        return;
    }
    // Build the root before adopting the host surface: the host's
    // `wait_for_vo_window` can't create its overlay surface until mpv's window
    // is ready.
    if !built {
        build_root();
        return;
    }
    if !adopted && let Some(host) = find_host_surface() {
        adopt_host_surface(host);
    }
}

// The host surface stays a subsurface, not an xdg role: an xdg role would
// collide with the host's own commits on it.
fn adopt_host_surface(host: Rc<WlSurface>) {
    let objs = with_shell(|sh| {
        if sh.host_adopted {
            return None;
        }
        Some((
            sh.subcompositor.clone()?,
            sh.spbm.clone()?,
            sh.viewporter.clone()?,
            sh.root_surface.clone()?,
            sh.cur_w.max(1),
            sh.cur_h.max(1),
        ))
    });
    let Some((subcompositor, spbm, viewporter, root, w, h)) = objs else {
        return;
    };

    // Created after mpv's subsurface, so it stacks above mpv by default.
    let sub = subcompositor.create_child::<WlSubsurface>();
    subcompositor.send_get_subsurface(&sub, &host, &root);
    sub.send_set_desync();
    sub.send_set_position(0, 0);

    // Transparent backdrop buffer scaled to the window, so overlay children map.
    let vp = viewporter.create_child::<WpViewport>();
    viewporter.send_get_viewport(&vp, &host);
    vp.send_set_destination(w, h);
    let buf = spbm.create_child::<WlBuffer>();
    spbm.send_create_u32_rgba_buffer(&buf, 0, 0, 0, 0);
    host.send_attach(Some(&buf), 0, 0);
    host.send_commit();
    // Adding the subsurface only takes effect on the parent's next commit; the
    // root otherwise commits only on a compositor configure, so map it now.
    root.send_commit();

    with_shell(|sh| {
        sh.host_surface = Some(host);
        sh.host_subsurface = Some(sub);
        sh.host_viewport = Some(vp);
        sh.host_adopted = true;
    });
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
                slf.send_bind(name, o.clone());
                with_shell(|sh| sh.compositor = Some(o));
            }
            WlSubcompositor::INTERFACE => {
                let o = state.create_object::<WlSubcompositor>(version.min(1));
                slf.send_bind(name, o.clone());
                with_shell(|sh| sh.subcompositor = Some(o));
            }
            XdgWmBase::INTERFACE => {
                let o = state.create_object::<XdgWmBase>(version.min(6));
                o.set_handler(ProxyWmBaseH);
                slf.send_bind(name, o.clone());
                with_shell(|sh| sh.wm_base = Some(o));
            }
            WpSinglePixelBufferManagerV1::INTERFACE => {
                let o = state.create_object::<WpSinglePixelBufferManagerV1>(version.min(1));
                slf.send_bind(name, o.clone());
                with_shell(|sh| sh.spbm = Some(o));
            }
            WpViewporter::INTERFACE => {
                let o = state.create_object::<WpViewporter>(version.min(1));
                slf.send_bind(name, o.clone());
                with_shell(|sh| sh.viewporter = Some(o));
            }
            ZxdgDecorationManagerV1::INTERFACE => {
                let o = state.create_object::<ZxdgDecorationManagerV1>(version.min(1));
                slf.send_bind(name, o.clone());
                with_shell(|sh| sh.decoration_manager = Some(o));
            }
            OrgKdeKwinServerDecorationPaletteManager::INTERFACE => {
                let o =
                    state.create_object::<OrgKdeKwinServerDecorationPaletteManager>(version.min(1));
                slf.send_bind(name, o.clone());
                with_shell(|sh| sh.palette_manager = Some(o));
            }
            _ => {}
        }
    }
}

struct ProxyWmBaseH;
impl XdgWmBaseHandler for ProxyWmBaseH {
    fn handle_ping(&mut self, slf: &Rc<XdgWmBase>, serial: u32) {
        // The compositor pings our own wm_base; mpv can't pong it, so we must.
        slf.send_pong(serial);
    }
}

struct RoundtripCb;
impl WlCallbackHandler for RoundtripCb {
    fn handle_done(&mut self, _slf: &Rc<WlCallback>, _data: u32) {
        let ok = with_shell(|sh| {
            sh.globals_ready = true;
            sh.compositor.is_some()
                && sh.subcompositor.is_some()
                && sh.wm_base.is_some()
                && sh.spbm.is_some()
                && sh.viewporter.is_some()
        });
        if !ok {
            eprintln!(
                "wlproxy: missing globals for root (need compositor, subcompositor, xdg_wm_base, single_pixel_buffer, viewporter)"
            );
        }
    }
}

fn build_root() {
    let objs = with_shell(|sh| {
        if !sh.demote_pending || sh.root_surface.is_some() {
            return None;
        }
        Some((
            sh.compositor.clone()?,
            sh.subcompositor.clone()?,
            sh.wm_base.clone()?,
            sh.mpv_surface.clone()?,
        ))
    });
    let Some((compositor, subcompositor, wm_base, mpv_surface)) = objs else {
        return;
    };

    let root_surface = {
        let s = compositor.create_child::<WlSurface>();
        compositor.send_create_surface(&s);
        s
    };

    let root_xdg = wm_base.create_child::<XdgSurface>();
    root_xdg.set_handler(RootXdgSurfaceH);
    wm_base.send_get_xdg_surface(&root_xdg, &root_surface);

    let root_tl = root_xdg.create_child::<XdgToplevel>();
    root_tl.set_handler(RootToplevelH);
    root_xdg.send_get_toplevel(&root_tl);

    if INITIAL_MAXIMIZED.load(Ordering::Acquire) {
        root_tl.send_set_maximized();
    }

    let mode = DECORATION_MODE.load(Ordering::Acquire);
    // Create the object even for Csd: KWin defaults an undecorated toplevel to
    // server-side, so CLIENT_SIDE must be requested to suppress its titlebar.
    let root_decoration = with_shell(|sh| sh.decoration_manager.clone()).map(|mgr| {
        let dec = mgr.create_child::<ZxdgToplevelDecorationV1>();
        mgr.send_get_toplevel_decoration(&dec, &root_tl);
        let wire_mode = if mode == DECO_CSD {
            ZxdgToplevelDecorationV1Mode::CLIENT_SIDE
        } else {
            ZxdgToplevelDecorationV1Mode::SERVER_SIDE
        };
        dec.send_set_mode(wire_mode);
        dec
    });

    let root_palette = (mode == DECO_SERVER_THEMED)
        .then(|| with_shell(|sh| sh.palette_manager.clone()))
        .flatten()
        .map(|mgr| {
            let pal = mgr.create_child::<OrgKdeKwinServerDecorationPalette>();
            mgr.send_create(&pal, &root_surface);
            pal
        });

    // Initial no-buffer commit to elicit the first configure.
    root_surface.send_commit();

    let sub = subcompositor.create_child::<WlSubsurface>();
    subcompositor.send_get_subsurface(&sub, &mpv_surface, &root_surface);
    sub.send_set_desync();
    sub.send_set_position(0, 0);

    // Empty input region so mpv's video is never a pointer target; the host
    // owns input on its overlay surface stacked above.
    let region = compositor.create_child::<WlRegion>();
    compositor.send_create_region(&region);
    mpv_surface.send_set_input_region(Some(&region));
    region.send_destroy();

    with_shell(|sh| {
        sh.root_surface = Some(root_surface);
        sh.root_xdg_surface = Some(root_xdg);
        sh.root_toplevel = Some(root_tl);
        sh.root_decoration = root_decoration;
        sh.root_palette = root_palette;
        sh.mpv_subsurface = Some(sub);
    });
}

fn single_pixel_rgba(rgb: u32) -> (u32, u32, u32, u32) {
    let chan = |c: u32| (c & 0xFF) * 0x0101_0101;
    (chan(rgb >> 16), chan(rgb >> 8), chan(rgb), 0xFFFF_FFFF)
}

fn fill_root(w: i32, h: i32) {
    let Some((compositor, spbm, viewporter, surface)) = with_shell(|sh| {
        Some((
            sh.compositor.clone()?,
            sh.spbm.clone()?,
            sh.viewporter.clone()?,
            sh.root_surface.clone()?,
        ))
    }) else {
        return;
    };
    let (w, h) = (w.max(1), h.max(1));

    let viewport = match with_shell(|sh| sh.root_viewport.clone()) {
        Some(vp) => vp,
        None => {
            let vp = viewporter.create_child::<WpViewport>();
            viewporter.send_get_viewport(&vp, &surface);
            with_shell(|sh| sh.root_viewport = Some(vp.clone()));
            vp
        }
    };
    viewport.send_set_destination(w, h);

    let (r, g, b, a) = single_pixel_rgba(BACKGROUND_RGBA.load(Ordering::Acquire));
    let buffer = spbm.create_child::<WlBuffer>();
    spbm.send_create_u32_rgba_buffer(&buffer, r, g, b, a);
    surface.send_attach(Some(&buffer), 0, 0);

    let region = compositor.create_child::<WlRegion>();
    compositor.send_create_region(&region);
    region.send_add(0, 0, w, h);
    surface.send_set_opaque_region(Some(&region));
    region.send_destroy();

    surface.send_commit();

    if let Some(old) = with_shell(|sh| sh.root_buffer.replace(buffer)) {
        old.send_destroy();
    }
}

fn refill_root_background() {
    let (w, h) = with_shell(|sh| (sh.cur_w, sh.cur_h));
    if w > 0 && h > 0 {
        fill_root(w, h);
    }
}

fn apply_titlebar_palette() {
    let Some(pal) = with_shell(|sh| sh.root_palette.clone()) else {
        return;
    };
    let guard = PENDING_PALETTE.lock();
    if let Some(path) = guard.as_ref().and_then(|p| p.to_str().ok()) {
        pal.send_set_palette(path);
    }
}

struct RootToplevelH;
impl XdgToplevelHandler for RootToplevelH {
    fn handle_configure(&mut self, _slf: &Rc<XdgToplevel>, width: i32, height: i32, states: &[u8]) {
        let mut fullscreen: c_int = 0;
        let mut maximized: c_int = 0;
        let mut suspended: c_int = 0;
        for chunk in states.chunks_exact(4) {
            let v = u32::from_ne_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
            if XdgToplevelState(v) == XdgToplevelState::FULLSCREEN {
                fullscreen = 1;
            } else if XdgToplevelState(v) == XdgToplevelState::MAXIMIZED {
                maximized = 1;
            } else if v == STATE_SUSPENDED {
                suspended = 1;
            }
        }
        let (init_w, init_h) = initial_size();
        let w = if width > 0 { width } else { init_w };
        let h = if height > 0 { height } else { init_h };
        {
            let mut p = PENDING.lock();
            p.have_configure = true;
            p.logical_w = w;
            p.logical_h = h;
            p.fullscreen = fullscreen;
            p.maximized = maximized;
        }
        fire_configure();
        fire_suspended(suspended);
        with_shell(|sh| {
            sh.cur_w = w;
            sh.cur_h = h;
            sh.cur_states = states.to_vec();
        });
    }

    fn handle_close(&mut self, _slf: &Rc<XdgToplevel>) {
        if let Some(cb) = *CLOSE_CB.lock() {
            cb();
        }
    }
}

struct RootXdgSurfaceH;
impl XdgSurfaceHandler for RootXdgSurfaceH {
    fn handle_configure(&mut self, slf: &Rc<XdgSurface>, serial: u32) {
        slf.send_ack_configure(serial);

        let (w, h, states) =
            with_shell(|sh| (sh.cur_w.max(1), sh.cur_h.max(1), sh.cur_states.clone()));

        let need_map = with_shell(|sh| !sh.root_mapped);

        slf.send_set_window_geometry(0, 0, w, h);
        fill_root(w, h);

        if need_map {
            with_shell(|sh| sh.root_mapped = true);
        }

        if let (Some(hv), Some(hs)) =
            with_shell(|sh| (sh.host_viewport.clone(), sh.host_surface.clone()))
        {
            hv.send_set_destination(w, h);
            hs.send_commit();
        }

        synth_mpv_configure(w, h, &states);
    }
}

fn synth_mpv_configure(w: i32, h: i32, states: &[u8]) {
    let (tl, xs, serial) = with_shell(|sh| {
        (
            sh.mpv_toplevel.clone(),
            sh.mpv_xdg_surface.clone(),
            sh.next_serial(),
        )
    });
    if let Some(tl) = tl {
        tl.send_configure(w, h, states);
    }
    if let Some(xs) = xs {
        xs.send_configure(serial);
    }
}
