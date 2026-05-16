//! Pure-forwarder Wayland proxy. Vet-only — no interception.
//!
//! mpv connects here instead of the real compositor (via WAYLAND_DISPLAY env).
//! Every byte forwards untouched in both directions. Default wl-proxy handler
//! impls forward all messages, so empty handler stubs suffice.
//!
//! We don't use `SimpleProxy` because it builds each per-client `State` using
//! the current process `WAYLAND_DISPLAY` env to find the upstream compositor —
//! but the caller overrides that env to OUR socket so mpv connects to us. We
//! must capture the original `WAYLAND_DISPLAY` here at `start` (before any
//! override) and pass it explicitly via `with_server_display_name`.

use std::ffi::CString;
use std::os::raw::c_char;
use std::rc::Rc;
use std::sync::mpsc;
use std::thread;

use error_reporter::Report;
use wl_proxy::acceptor::Acceptor;
use wl_proxy::baseline::Baseline;
use wl_proxy::client::ClientHandler;
use wl_proxy::protocols::wayland::wl_display::WlDisplayHandler;
use wl_proxy::state::State;

pub struct Proxy {
    display_name: CString,
    _thread: thread::JoinHandle<()>,
}

struct PassthroughDisplay;
impl WlDisplayHandler for PassthroughDisplay {}

struct NoopClient;
impl ClientHandler for NoopClient {
    fn disconnected(self: Box<Self>) {}
}

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

fn run_client(socket: std::os::fd::OwnedFd, upstream: Option<String>) {
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
    client.display().set_handler(PassthroughDisplay);
    while state.is_not_destroyed() {
        if let Err(e) = state.dispatch_blocking() {
            eprintln!("wlproxy: dispatch: {}", Report::new(e));
            return;
        }
    }
}

/// Returns the WAYLAND_DISPLAY value clients should connect to (e.g. "wayland-1").
/// Returns null if `p` is null. Pointer is valid until `jfn_wlproxy_stop`.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_wlproxy_display_name(p: *const Proxy) -> *const c_char {
    if p.is_null() {
        return std::ptr::null();
    }
    unsafe { (*p).display_name.as_ptr() }
}

/// Drop the proxy handle. The listener thread is detached; OS cleans up on
/// process exit. Safe to call with null.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_wlproxy_stop(p: *mut Proxy) {
    if p.is_null() {
        return;
    }
    unsafe { drop(Box::from_raw(p)) };
}
