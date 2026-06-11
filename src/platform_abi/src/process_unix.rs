//! Unix defaults for the `Platform` process-lifecycle methods: SIGINT/SIGTERM
//! shutdown handlers and the AF_UNIX single-instance gate.

use std::sync::OnceLock;

// =====================================================================
// Shutdown signals
// =====================================================================

use libc::{SIGINT, SIGTERM, c_int, sigaction, sigemptyset};

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
unsafe extern "C" fn on_shutdown_signal(_sig: c_int) {
    if let Some(cb) = SHUTDOWN_CB.get() {
        cb();
    }
}

struct SignalGuard {
    prev_int: sigaction,
    prev_term: sigaction,
}

impl Drop for SignalGuard {
    fn drop(&mut self) {
        unsafe {
            libc::sigaction(SIGINT, &self.prev_int, std::ptr::null_mut());
            libc::sigaction(SIGTERM, &self.prev_term, std::ptr::null_mut());
        }
    }
}

/// # Safety
/// `handler` must be async-signal-safe: it runs from inside a `sigaction`
/// handler installed on SIGINT/SIGTERM.
unsafe fn install_guard(handler: unsafe extern "C" fn(c_int)) -> SignalGuard {
    let mut sa: sigaction = unsafe { std::mem::zeroed() };
    sa.sa_sigaction = handler as usize;
    unsafe { sigemptyset(&mut sa.sa_mask) };

    let mut prev_int: sigaction = unsafe { std::mem::zeroed() };
    let mut prev_term: sigaction = unsafe { std::mem::zeroed() };
    unsafe {
        libc::sigaction(SIGINT, &sa, &mut prev_int);
        libc::sigaction(SIGTERM, &sa, &mut prev_term);
    }
    SignalGuard {
        prev_int,
        prev_term,
    }
}

// =====================================================================
// Single-instance gate (AF_UNIX SOCK_STREAM)
// =====================================================================

mod single_instance {
    use libc::getuid;
    use libc::{
        AF_UNIX, ECONNREFUSED, ENOENT, POLLIN, SOCK_STREAM, c_char, c_int, c_void, close, pipe,
        poll, pollfd, sockaddr_un,
    };
    use parking_lot::Mutex;
    use std::ffi::CString;
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
        let file_name = format!("jellyfin-desktop-{instance_id}.sock");
        if let Ok(dir) = std::env::var("XDG_RUNTIME_DIR")
            && !dir.is_empty()
        {
            let mut p = PathBuf::from(dir);
            p.push(file_name);
            return p;
        }
        let uid = unsafe { getuid() };
        PathBuf::from(format!("/tmp/jellyfin-desktop-{uid}-{instance_id}.sock"))
    }

    fn fill_sockaddr(path: &std::ffi::CStr) -> Option<sockaddr_un> {
        let mut addr: sockaddr_un = unsafe { std::mem::zeroed() };
        addr.sun_family = AF_UNIX as _;
        let bytes = path.to_bytes_with_nul();
        if bytes.len() > addr.sun_path.len() {
            return None;
        }
        for (dst, src) in addr.sun_path.iter_mut().zip(bytes.iter()) {
            *dst = *src as c_char;
        }
        Some(addr)
    }

    pub fn try_signal_existing(instance_id: &str) -> bool {
        let path = socket_path(instance_id);
        let Ok(cpath) = CString::new(path.as_os_str().as_encoded_bytes()) else {
            return false;
        };
        let Some(addr) = fill_sockaddr(&cpath) else {
            return false;
        };

        let fd = unsafe { libc::socket(AF_UNIX, SOCK_STREAM, 0) };
        if fd < 0 {
            return false;
        }
        let rc = unsafe {
            libc::connect(
                fd,
                &addr as *const _ as *const libc::sockaddr,
                std::mem::size_of::<sockaddr_un>() as libc::socklen_t,
            )
        };
        if rc < 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            unsafe { close(fd) };
            if err == ECONNREFUSED || err == ENOENT {
                // Stale socket — remove so the new owner can bind.
                unsafe { libc::unlink(cpath.as_ptr()) };
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
        let bytes = msg.as_bytes();
        unsafe {
            libc::write(fd, bytes.as_ptr() as *const c_void, bytes.len());
            close(fd);
        }
        true
    }

    fn listener_loop(cb: Callback) {
        let listen_fd = LISTEN_FD.load(Ordering::Acquire);
        let wake_fd = WAKE_READ.load(Ordering::Acquire);
        let mut pfds = [
            pollfd {
                fd: listen_fd,
                events: POLLIN,
                revents: 0,
            },
            pollfd {
                fd: wake_fd,
                events: POLLIN,
                revents: 0,
            },
        ];
        while RUNNING.load(Ordering::Acquire) {
            let n = unsafe { poll(pfds.as_mut_ptr(), 2, -1) };
            if n < 0 {
                break;
            }
            if pfds[1].revents & POLLIN != 0 {
                break;
            }
            if pfds[0].revents & POLLIN == 0 {
                continue;
            }
            let client =
                unsafe { libc::accept(listen_fd, std::ptr::null_mut(), std::ptr::null_mut()) };
            if client < 0 {
                continue;
            }
            let mut buf = [0u8; 256];
            let n = unsafe { libc::read(client, buf.as_mut_ptr() as *mut c_void, buf.len() - 1) };
            unsafe { close(client) };
            if n <= 0 {
                continue;
            }
            let line = &buf[..n as usize];
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
        let Ok(cpath) = CString::new(path.as_os_str().as_encoded_bytes()) else {
            return false;
        };
        let Some(addr) = fill_sockaddr(&cpath) else {
            return false;
        };

        let fd = unsafe { libc::socket(AF_UNIX, SOCK_STREAM, 0) };
        if fd < 0 {
            return false;
        }
        let rc = unsafe {
            libc::bind(
                fd,
                &addr as *const _ as *const libc::sockaddr,
                std::mem::size_of::<sockaddr_un>() as libc::socklen_t,
            )
        };
        if rc < 0 {
            unsafe { close(fd) };
            return false;
        }
        if unsafe { libc::listen(fd, 2) } < 0 {
            unsafe { close(fd) };
            unsafe { libc::unlink(cpath.as_ptr()) };
            return false;
        }

        let mut pipefds: [c_int; 2] = [-1, -1];
        if unsafe { pipe(pipefds.as_mut_ptr()) } < 0 {
            unsafe { close(fd) };
            unsafe { libc::unlink(cpath.as_ptr()) };
            return false;
        }

        LISTEN_FD.store(fd, Ordering::Release);
        WAKE_READ.store(pipefds[0], Ordering::Release);
        WAKE_WRITE.store(pipefds[1], Ordering::Release);
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
            let buf = b"x";
            unsafe {
                libc::write(wake_write, buf.as_ptr() as *const c_void, 1);
            }
        }
        if let Some(h) = THREAD.lock().take()
            && let Err(e) = h.join()
        {
            eprintln!("[single-instance] listener thread panicked: {e:?}");
        }
        let path = socket_path(instance_id);
        if let Ok(cpath) = CString::new(path.as_os_str().as_encoded_bytes()) {
            unsafe { libc::unlink(cpath.as_ptr()) };
        }
        let listen = LISTEN_FD.swap(-1, Ordering::AcqRel);
        if listen >= 0 {
            unsafe { close(listen) };
        }
        let r = WAKE_READ.swap(-1, Ordering::AcqRel);
        if r >= 0 {
            unsafe { close(r) };
        }
        if wake_write >= 0 {
            unsafe { close(wake_write) };
        }
    }
}

pub use single_instance::{start_listener, stop_listener, try_signal_existing};
