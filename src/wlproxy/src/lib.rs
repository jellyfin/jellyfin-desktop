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

use std::cell::RefCell;
use std::collections::VecDeque;
use std::ffi::CString;
use std::os::fd::OwnedFd;
use std::os::raw::{c_char, c_int};
use std::rc::Rc;
use std::sync::{Mutex, mpsc};
use std::thread;
use std::time::Duration;

use error_reporter::Report;
use wl_proxy::acceptor::Acceptor;
use wl_proxy::baseline::Baseline;
use wl_proxy::client::ClientHandler;
use wl_proxy::object::{ConcreteObject, Object, ObjectCoreApi, ObjectRcUtils};
use wl_proxy::protocols::fractional_scale_v1::wp_fractional_scale_manager_v1::{
    WpFractionalScaleManagerV1, WpFractionalScaleManagerV1Handler,
};
use wl_proxy::protocols::fractional_scale_v1::wp_fractional_scale_v1::{
    WpFractionalScaleV1, WpFractionalScaleV1Handler,
};
use wl_proxy::protocols::wayland::wl_display::{WlDisplay, WlDisplayHandler};
use wl_proxy::protocols::wayland::wl_output::WlOutput;
use wl_proxy::protocols::wayland::wl_registry::{WlRegistry, WlRegistryHandler};
use wl_proxy::protocols::wayland::wl_surface::WlSurface;
use wl_proxy::protocols::xdg_shell::xdg_surface::{XdgSurface, XdgSurfaceHandler};
use wl_proxy::protocols::xdg_shell::xdg_toplevel::{
    XdgToplevel, XdgToplevelHandler, XdgToplevelState,
};
use wl_proxy::protocols::xdg_shell::xdg_wm_base::{XdgWmBase, XdgWmBaseHandler};
use wl_proxy::state::State;

pub struct Proxy {
    display_name: CString,
    _thread: thread::JoinHandle<()>,
}

type ConfigureCb = extern "C" fn(c_int, c_int, c_int);
type ScaleCb = extern "C" fn(c_int);

// Single proxy per process — callbacks are global. Fire from the per-client
// proxy thread; the C side guards against thread-safety with its own mutex.
static CONFIGURE_CB: Mutex<Option<ConfigureCb>> = Mutex::new(None);
static SCALE_CB: Mutex<Option<ScaleCb>> = Mutex::new(None);

enum HostCommand {
    SetFullscreen(bool),
    SetMaximized(bool),
}

static COMMANDS: Mutex<VecDeque<HostCommand>> = Mutex::new(VecDeque::new());

thread_local! {
    // Per-client thread stores the XdgToplevel it manages so the command
    // drain (which runs on the same thread) can issue requests on it.
    static TOPLEVEL: RefCell<Option<Rc<XdgToplevel>>> = const { RefCell::new(None) };
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
    scale_120: u32,
}

static PENDING: Mutex<PendingConfigure> = Mutex::new(PendingConfigure {
    have_configure: false,
    logical_w: 0,
    logical_h: 0,
    fullscreen: 0,
    scale_120: 120,
});

fn fire_configure() {
    let p = PENDING.lock().unwrap();
    if !p.have_configure {
        return;
    }
    // Round half-up: (logical * scale_120 + WAYLAND_SCALE_FACTOR/2) / WAYLAND_SCALE_FACTOR.
    let pw = ((p.logical_w as i64 * p.scale_120 as i64 + 60) / 120) as c_int;
    let ph = ((p.logical_h as i64 * p.scale_120 as i64 + 60) / 120) as c_int;
    let fs = p.fullscreen;
    drop(p);
    if let Some(cb) = *CONFIGURE_CB.lock().unwrap() {
        cb(pw, ph, fs);
    }
}

// =========================================================================
// FFI
// =========================================================================

/// Start the proxy. Spawns a listener thread that owns the `Acceptor` (which
/// is `!Send` because it holds `Rc` internally, so it must be constructed
/// inside the thread). The thread hands the listening socket name back via a
/// channel before entering its blocking accept loop.
///
/// Returns null on failure.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_wlproxy_start() -> *mut Proxy {
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
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_wlproxy_display_name(p: *const Proxy) -> *const c_char {
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
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_wlproxy_stop(p: *mut Proxy) {
    if p.is_null() {
        return;
    }
    unsafe { drop(Box::from_raw(p)) };
}

/// Register the xdg_toplevel.configure interception callback.
///
/// Fires from the proxy's per-client thread whenever the compositor sends an
/// `xdg_toplevel.configure` event. Arguments are `(width, height, fullscreen)`
/// — fullscreen is 1 if the configure's states[] array contains
/// `XDG_TOPLEVEL_STATE_FULLSCREEN`, 0 otherwise. width/height are physical
/// pixels (scaled by the current `scale_120 / 120` factor).
///
/// The event still forwards to mpv after the callback runs.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_wlproxy_set_configure_callback(cb: ConfigureCb) {
    *CONFIGURE_CB.lock().unwrap() = Some(cb);
}

/// Register the wp_fractional_scale_v1.preferred_scale callback.
///
/// Argument is the scale numerator over `WAYLAND_SCALE_FACTOR=120` (so 120 =
/// 1.0x, 180 = 1.5x, 240 = 2.0x). Fires once whenever the compositor sends a
/// new preferred scale for the toplevel's surface.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_wlproxy_set_scale_callback(cb: ScaleCb) {
    *SCALE_CB.lock().unwrap() = Some(cb);
}

/// Queue an xdg_toplevel.set_fullscreen / unset_fullscreen request. Applied
/// from the proxy's per-client thread on its next dispatch iteration.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_wlproxy_set_fullscreen(enable: c_int) {
    COMMANDS
        .lock()
        .unwrap()
        .push_back(HostCommand::SetFullscreen(enable != 0));
}

/// Queue an xdg_toplevel.set_maximized / unset_maximized request. Applied
/// from the proxy's per-client thread on its next dispatch iteration.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_wlproxy_set_maximized(enable: c_int) {
    COMMANDS
        .lock()
        .unwrap()
        .push_back(HostCommand::SetMaximized(enable != 0));
}

// =========================================================================
// Listener / per-client thread
// =========================================================================

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
    let mut builder = State::builder(Baseline::ALL_OF_THEM).with_log_prefix("jfn");
    if let Some(name) = &upstream {
        builder = builder.with_server_display_name(name);
    }
    let state = match builder.build() {
        Ok(s) => s,
        Err(e) => {
            eprintln!("wlproxy: State::build: {}", Report::new(e));
            return;
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
    }
}

fn drain_host_commands() {
    let cmds: Vec<HostCommand> = COMMANDS.lock().unwrap().drain(..).collect();
    if cmds.is_empty() {
        return;
    }
    TOPLEVEL.with(|t| {
        let tl_ref = t.borrow();
        let Some(tl) = tl_ref.as_ref() else {
            // No toplevel in this thread (e.g. early commands queued before
            // mpv got to xdg_surface.get_toplevel). Drop silently — the
            // commands aren't replayed, but for the current contract (host
            // calls only after the window exists) that's fine.
            return;
        };
        for cmd in cmds {
            match cmd {
                HostCommand::SetFullscreen(true) => tl.send_set_fullscreen(None),
                HostCommand::SetFullscreen(false) => tl.send_unset_fullscreen(),
                HostCommand::SetMaximized(true) => tl.send_set_maximized(),
                HostCommand::SetMaximized(false) => tl.send_unset_maximized(),
            }
        }
    });
}

// =========================================================================
// Handler chain: WlDisplay → WlRegistry → XdgWmBase → XdgSurface → XdgToplevel
// =========================================================================

struct NoopClient;
impl ClientHandler for NoopClient {
    fn disconnected(self: Box<Self>) {}
}

struct DisplayH;
impl WlDisplayHandler for DisplayH {
    fn handle_get_registry(&mut self, slf: &Rc<WlDisplay>, registry: &Rc<WlRegistry>) {
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
            _ => {}
        }
        slf.send_bind(name, id);
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
        PENDING.lock().unwrap().scale_120 = scale;
        if let Some(cb) = *SCALE_CB.lock().unwrap() {
            cb(scale as c_int);
        }
        fire_configure();
        slf.send_preferred_scale(scale);
    }
}

struct WmBaseH;
impl XdgWmBaseHandler for WmBaseH {
    fn handle_get_xdg_surface(
        &mut self,
        slf: &Rc<XdgWmBase>,
        id: &Rc<XdgSurface>,
        surface: &Rc<WlSurface>,
    ) {
        id.set_handler(SurfaceH);
        slf.send_get_xdg_surface(id, surface);
    }
}

struct SurfaceH;
impl XdgSurfaceHandler for SurfaceH {
    fn handle_get_toplevel(&mut self, slf: &Rc<XdgSurface>, id: &Rc<XdgToplevel>) {
        id.set_handler(ToplevelH);
        TOPLEVEL.with(|t| *t.borrow_mut() = Some(id.clone()));
        slf.send_get_toplevel(id);
    }

    // Eat mpv's window-geometry hint. On Wayland the host (C++) is the sole
    // authority for window state; mpv shouldn't be telling the compositor
    // anything about geometry.
    fn handle_set_window_geometry(
        &mut self,
        _slf: &Rc<XdgSurface>,
        _x: i32,
        _y: i32,
        _width: i32,
        _height: i32,
    ) {
    }
}

struct ToplevelH;
impl XdgToplevelHandler for ToplevelH {
    // ===== Eat mpv's state-change requests =====
    // C++ drives all window state via jfn_wlproxy_set_fullscreen /
    // set_maximized (which fire send_* from the proxy directly). mpv's
    // outgoing state requests are dropped so they can't race the host.

    fn handle_set_fullscreen(&mut self, _slf: &Rc<XdgToplevel>, _output: Option<&Rc<WlOutput>>) {}
    fn handle_unset_fullscreen(&mut self, _slf: &Rc<XdgToplevel>) {}
    fn handle_set_maximized(&mut self, _slf: &Rc<XdgToplevel>) {}
    fn handle_unset_maximized(&mut self, _slf: &Rc<XdgToplevel>) {}
    fn handle_set_minimized(&mut self, _slf: &Rc<XdgToplevel>) {}
    fn handle_set_min_size(&mut self, _slf: &Rc<XdgToplevel>, _w: i32, _h: i32) {}
    fn handle_set_max_size(&mut self, _slf: &Rc<XdgToplevel>, _w: i32, _h: i32) {}

    fn handle_configure(&mut self, slf: &Rc<XdgToplevel>, width: i32, height: i32, states: &[u8]) {
        // states is a wire-encoded wl_array of uint32 XdgToplevelState values
        // in native byte order. Scan for FULLSCREEN; ignore other states.
        let mut fullscreen: c_int = 0;
        for chunk in states.chunks_exact(4) {
            let v = u32::from_ne_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
            if XdgToplevelState(v) == XdgToplevelState::FULLSCREEN {
                fullscreen = 1;
                break;
            }
        }
        {
            let mut p = PENDING.lock().unwrap();
            p.have_configure = true;
            p.logical_w = width;
            p.logical_h = height;
            p.fullscreen = fullscreen;
        }
        fire_configure();
        slf.send_configure(width, height, states);
    }
}
