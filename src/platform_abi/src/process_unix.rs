//! Unix defaults for the `Platform` process-lifecycle methods: SIGINT/SIGTERM
//! shutdown handlers and the AF_UNIX single-instance gate.

use std::sync::OnceLock;

// =====================================================================
// Shutdown signals
// =====================================================================

use std::ffi::c_int;

use nix::sys::signal::{SaFlags, SigAction, SigHandler, SigSet, Signal, sigaction};

// Slot stays until process exit; the guard's Drop restores the original
// dispositions.
static GUARD: OnceLock<SignalGuard> = OnceLock::new();
static SHUTDOWN_CB: OnceLock<fn()> = OnceLock::new();

pub fn install_shutdown(on_shutdown: fn()) {
    // Set before arming: the handler dereferences this, so it must be live by
    // the time a signal can fire.
    let _ = SHUTDOWN_CB.set(on_shutdown);
    let g = unsafe { install_guard(on_shutdown_signal) };
    let _ = GUARD.set(g);
}

// Async-signal-safe by contract: reads an already-set OnceLock fn pointer and
// calls it. The callback (jfn_shutdown_initiate) only wakes the manager; the
// close/drain is orchestrated off-thread.
extern "C" fn on_shutdown_signal(_sig: c_int) {
    if let Some(cb) = SHUTDOWN_CB.get() {
        cb();
    }
}

struct SignalGuard {
    prev_int: Option<SigAction>,
    prev_term: Option<SigAction>,
}

impl Drop for SignalGuard {
    fn drop(&mut self) {
        if let Some(prev) = &self.prev_int {
            let _ = unsafe { sigaction(Signal::SIGINT, prev) };
        }
        if let Some(prev) = &self.prev_term {
            let _ = unsafe { sigaction(Signal::SIGTERM, prev) };
        }
    }
}

/// # Safety
/// `handler` must be async-signal-safe: it runs from inside a `sigaction`
/// handler installed on SIGINT/SIGTERM.
unsafe fn install_guard(handler: extern "C" fn(c_int)) -> SignalGuard {
    let sa = SigAction::new(
        SigHandler::Handler(handler),
        SaFlags::empty(),
        SigSet::empty(),
    );
    SignalGuard {
        prev_int: unsafe { sigaction(Signal::SIGINT, &sa) }.ok(),
        prev_term: unsafe { sigaction(Signal::SIGTERM, &sa) }.ok(),
    }
}

// =====================================================================
// Single-instance gate (AF_UNIX SOCK_STREAM)
// =====================================================================

mod single_instance {
    use nix::errno::Errno;
    use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
    use nix::sys::socket::{
        AddressFamily, Backlog, SockFlag, SockType, UnixAddr, accept, bind, connect, listen, socket,
    };
    use nix::unistd::{close, getuid, pipe, read, unlink, write};
    use parking_lot::Mutex;
    use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, IntoRawFd, OwnedFd};
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
    use std::thread::{self, JoinHandle};

    use super::super::Callback;

    static LISTEN_FD: AtomicI32 = AtomicI32::new(-1);
    static WAKE_READ: AtomicI32 = AtomicI32::new(-1);
    static WAKE_WRITE: AtomicI32 = AtomicI32::new(-1);
    static RUNNING: AtomicBool = AtomicBool::new(false);
    static THREAD: Mutex<Option<JoinHandle<()>>> = Mutex::new(None);

    fn socket_path(instance_id: &str) -> PathBuf {
        let file_name = format!("jellium-desktop-{instance_id}.sock");
        if let Ok(dir) = std::env::var("XDG_RUNTIME_DIR")
            && !dir.is_empty()
        {
            let mut p = PathBuf::from(dir);
            p.push(file_name);
            return p;
        }
        let uid = getuid();
        PathBuf::from(format!("/tmp/jellium-desktop-{uid}-{instance_id}.sock"))
    }

    pub fn try_signal_existing(instance_id: &str) -> bool {
        let path = socket_path(instance_id);
        let Ok(addr) = UnixAddr::new(&path) else {
            return false;
        };

        let Ok(fd) = socket(
            AddressFamily::Unix,
            SockType::Stream,
            SockFlag::empty(),
            None,
        ) else {
            return false;
        };
        if let Err(err) = connect(fd.as_raw_fd(), &addr) {
            if matches!(err, Errno::ECONNREFUSED | Errno::ENOENT) {
                // Stale socket — remove so the new owner can bind.
                let _ = unlink(&path);
            }
            return false;
        }

        let mut msg = String::from("raise");
        if let Ok(token) = std::env::var("XDG_ACTIVATION_TOKEN")
            && !token.is_empty()
        {
            msg.push(' ');
            msg.push_str(&token);
        }
        msg.push('\n');
        let _ = write(&fd, msg.as_bytes());
        true
    }

    fn listener_loop(cb: Callback) {
        let listen_fd = LISTEN_FD.load(Ordering::Acquire);
        let wake_fd = WAKE_READ.load(Ordering::Acquire);
        let listen_bfd = unsafe { BorrowedFd::borrow_raw(listen_fd) };
        let wake_bfd = unsafe { BorrowedFd::borrow_raw(wake_fd) };
        let mut pfds = [
            PollFd::new(listen_bfd, PollFlags::POLLIN),
            PollFd::new(wake_bfd, PollFlags::POLLIN),
        ];
        while RUNNING.load(Ordering::Acquire) {
            if poll(&mut pfds, PollTimeout::NONE).is_err() {
                break;
            }
            let readable =
                |pfd: &PollFd| pfd.revents().is_some_and(|r| r.contains(PollFlags::POLLIN));
            if readable(&pfds[1]) {
                break;
            }
            if !readable(&pfds[0]) {
                continue;
            }
            let Ok(client) = accept(listen_fd) else {
                continue;
            };
            let client = unsafe { OwnedFd::from_raw_fd(client) };
            let mut buf = [0u8; 256];
            let n = read(&client, &mut buf);
            drop(client);
            let Ok(n) = n else {
                continue;
            };
            if n == 0 {
                continue;
            }
            let line = &buf[..n];
            if !line.starts_with(b"raise") {
                continue;
            }
            let token = if let Some(rest) = line.strip_prefix(b"raise ") {
                let mut end = rest.len();
                while end > 0 && (rest[end - 1] == b'\n' || rest[end - 1] == b'\r') {
                    end -= 1;
                }
                String::from_utf8_lossy(&rest[..end]).into_owned()
            } else {
                String::new()
            };
            cb(&token);
        }
    }

    pub fn start_listener(instance_id: &str, cb: Callback) -> bool {
        if RUNNING.load(Ordering::Acquire) {
            return true;
        }
        let path = socket_path(instance_id);
        let Ok(addr) = UnixAddr::new(&path) else {
            return false;
        };

        let Ok(fd) = socket(
            AddressFamily::Unix,
            SockType::Stream,
            SockFlag::empty(),
            None,
        ) else {
            return false;
        };
        if bind(fd.as_raw_fd(), &addr).is_err() {
            return false;
        }
        let backlog_ok = Backlog::new(2).is_ok_and(|b| listen(&fd, b).is_ok());
        if !backlog_ok {
            let _ = unlink(&path);
            return false;
        }

        let Ok((wake_read, wake_write)) = pipe() else {
            let _ = unlink(&path);
            return false;
        };

        LISTEN_FD.store(fd.into_raw_fd(), Ordering::Release);
        WAKE_READ.store(wake_read.into_raw_fd(), Ordering::Release);
        WAKE_WRITE.store(wake_write.into_raw_fd(), Ordering::Release);
        RUNNING.store(true, Ordering::Release);

        let handle = thread::spawn(move || listener_loop(cb));
        *THREAD.lock() = Some(handle);
        true
    }

    pub fn stop_listener(instance_id: &str) {
        if !RUNNING.swap(false, Ordering::AcqRel) {
            return;
        }
        let wake_write = WAKE_WRITE.swap(-1, Ordering::AcqRel);
        if wake_write >= 0 {
            let bfd = unsafe { BorrowedFd::borrow_raw(wake_write) };
            let _ = write(bfd, b"x");
        }
        if let Some(h) = THREAD.lock().take()
            && let Err(e) = h.join()
        {
            eprintln!("[single-instance] listener thread panicked: {e:?}");
        }
        let _ = unlink(&socket_path(instance_id));
        let listen = LISTEN_FD.swap(-1, Ordering::AcqRel);
        if listen >= 0 {
            let _ = close(listen);
        }
        let r = WAKE_READ.swap(-1, Ordering::AcqRel);
        if r >= 0 {
            let _ = close(r);
        }
        if wake_write >= 0 {
            let _ = close(wake_write);
        }
    }
}

pub use single_instance::{start_listener, stop_listener, try_signal_existing};
