//! Single-instance gate.
//!
//! Lets a second invocation of the app raise the existing window instead
//! of starting a fresh process. Implementation per platform:
//!
//! - Linux/macOS: AF_UNIX SOCK_STREAM socket under `$XDG_RUNTIME_DIR` or
//!   `$TMPDIR`. The new process writes `raise [token]\n` and the running
//!   process's listener thread invokes a callback with the activation
//!   token (used by xdg-activation-v1 to focus the window).
//! - Windows: named pipe `\\.\pipe\jellyfin-desktop`. The new process
//!   writes `raise\n` (no activation token concept on Windows).

use std::ffi::c_void;
use std::os::raw::c_char;

#[derive(Clone, Copy)]
struct Callback {
    cb: unsafe extern "C" fn(*const c_char, *mut c_void),
    userdata: *mut c_void,
}

// Pointer is opaque to Rust; C caller guarantees it stays valid until
// stop_listener returns.
unsafe impl Send for Callback {}
unsafe impl Sync for Callback {}

#[cfg(unix)]
mod imp {
    use super::Callback;
    use libc::{
        AF_UNIX, ECONNREFUSED, ENOENT, POLLIN, SOCK_STREAM, c_char, c_int, c_void, close, getuid,
        pipe, poll, pollfd, sockaddr_un,
    };
    use std::ffi::{CStr, CString};
    use std::path::PathBuf;
    use std::sync::Mutex;
    use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
    use std::thread::{self, JoinHandle};

    static LISTEN_FD: AtomicI32 = AtomicI32::new(-1);
    static WAKE_READ: AtomicI32 = AtomicI32::new(-1);
    static WAKE_WRITE: AtomicI32 = AtomicI32::new(-1);
    static RUNNING: AtomicBool = AtomicBool::new(false);
    static THREAD: Mutex<Option<JoinHandle<()>>> = Mutex::new(None);

    fn socket_path() -> PathBuf {
        #[cfg(target_os = "macos")]
        {
            if let Ok(tmpdir) = std::env::var("TMPDIR")
                && !tmpdir.is_empty()
            {
                let mut p = PathBuf::from(tmpdir);
                p.push("jellyfin-desktop.sock");
                return p;
            }
            PathBuf::from("/tmp/jellyfin-desktop.sock")
        }
        #[cfg(not(target_os = "macos"))]
        {
            if let Ok(dir) = std::env::var("XDG_RUNTIME_DIR")
                && !dir.is_empty()
            {
                let mut p = PathBuf::from(dir);
                p.push("jellyfin-desktop.sock");
                return p;
            }
            let uid = unsafe { getuid() };
            PathBuf::from(format!("/tmp/jellyfin-desktop-{uid}.sock"))
        }
    }

    fn fill_sockaddr(path: &CStr) -> Option<sockaddr_un> {
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

    pub fn try_signal_existing() -> bool {
        let path = socket_path();
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
                CString::new(&rest[..end]).unwrap_or_default()
            } else {
                CString::default()
            };
            unsafe { (cb.cb)(token.as_ptr(), cb.userdata) };
        }
    }

    pub fn start_listener(cb: Callback) -> bool {
        if RUNNING.load(Ordering::Acquire) {
            return true;
        }
        let path = socket_path();
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
        *THREAD.lock().unwrap() = Some(handle);
        true
    }

    pub fn stop_listener() {
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
        if let Some(h) = THREAD.lock().unwrap().take() {
            let _ = h.join();
        }
        let path = socket_path();
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

#[cfg(windows)]
mod imp {
    use super::Callback;
    use std::ffi::CString;
    use std::sync::Mutex;
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
    use std::thread::{self, JoinHandle};
    use windows_sys::Win32::Foundation::{CloseHandle, GENERIC_WRITE, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileA, OPEN_EXISTING, ReadFile, WriteFile,
    };
    use windows_sys::Win32::System::IO::{CancelIo, OVERLAPPED};
    use windows_sys::Win32::System::Pipes::{
        ConnectNamedPipe, CreateNamedPipeA, DisconnectNamedPipe, PIPE_ACCESS_INBOUND,
        PIPE_READMODE_BYTE, PIPE_TYPE_BYTE, PIPE_WAIT,
    };
    use windows_sys::Win32::System::Threading::{
        CreateEventA, INFINITE, SetEvent, WaitForMultipleObjects,
    };

    type HANDLE = *mut std::ffi::c_void;
    const WAIT_OBJECT_0: u32 = 0;
    const PIPE_NAME: &[u8] = b"\\\\.\\pipe\\jellyfin-desktop\0";
    const FILE_FLAG_OVERLAPPED: u32 = 0x40000000;

    static RUNNING: AtomicBool = AtomicBool::new(false);
    static SHUTDOWN_EVENT: AtomicUsize = AtomicUsize::new(0);
    static THREAD: Mutex<Option<JoinHandle<()>>> = Mutex::new(None);

    pub fn try_signal_existing() -> bool {
        let pipe = unsafe {
            CreateFileA(
                PIPE_NAME.as_ptr(),
                GENERIC_WRITE,
                0,
                std::ptr::null(),
                OPEN_EXISTING,
                0,
                std::ptr::null_mut(),
            )
        };
        if pipe == INVALID_HANDLE_VALUE {
            return false;
        }
        let msg = b"raise\n";
        let mut written: u32 = 0;
        unsafe {
            WriteFile(
                pipe,
                msg.as_ptr(),
                msg.len() as u32,
                &mut written,
                std::ptr::null_mut(),
            );
            CloseHandle(pipe);
        }
        true
    }

    fn listener_loop(cb: Callback) {
        let shutdown = SHUTDOWN_EVENT.load(Ordering::Acquire) as HANDLE;
        while RUNNING.load(Ordering::Acquire) {
            let pipe = unsafe {
                CreateNamedPipeA(
                    PIPE_NAME.as_ptr(),
                    PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                    1,
                    0,
                    256,
                    0,
                    std::ptr::null(),
                )
            };
            if pipe == INVALID_HANDLE_VALUE {
                break;
            }

            let event = unsafe { CreateEventA(std::ptr::null(), 1, 0, std::ptr::null()) };
            let mut overlapped: OVERLAPPED = unsafe { std::mem::zeroed() };
            overlapped.hEvent = event;
            unsafe { ConnectNamedPipe(pipe, &mut overlapped) };

            let handles = [event, shutdown];
            let result = unsafe { WaitForMultipleObjects(2, handles.as_ptr(), 0, INFINITE) };
            unsafe { CloseHandle(event) };

            if result == WAIT_OBJECT_0 + 1 {
                unsafe {
                    CancelIo(pipe);
                    DisconnectNamedPipe(pipe);
                    CloseHandle(pipe);
                }
                break;
            }

            let mut buf = [0u8; 256];
            let mut read: u32 = 0;
            let ok = unsafe {
                ReadFile(
                    pipe,
                    buf.as_mut_ptr(),
                    (buf.len() - 1) as u32,
                    &mut read,
                    std::ptr::null_mut(),
                )
            };
            if ok != 0 && read > 0 {
                let line = &buf[..read as usize];
                if line.windows(5).any(|w| w == b"raise") {
                    let empty = CString::default();
                    unsafe { (cb.cb)(empty.as_ptr(), cb.userdata) };
                }
            }
            unsafe {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
        }
    }

    pub fn start_listener(cb: Callback) -> bool {
        if RUNNING.load(Ordering::Acquire) {
            return true;
        }
        let event = unsafe { CreateEventA(std::ptr::null(), 1, 0, std::ptr::null()) };
        if event.is_null() {
            return false;
        }
        SHUTDOWN_EVENT.store(event as usize, Ordering::Release);
        RUNNING.store(true, Ordering::Release);
        let handle = thread::spawn(move || listener_loop(cb));
        *THREAD.lock().unwrap() = Some(handle);
        true
    }

    pub fn stop_listener() {
        if !RUNNING.swap(false, Ordering::AcqRel) {
            return;
        }
        let event = SHUTDOWN_EVENT.swap(0, Ordering::AcqRel) as HANDLE;
        if !event.is_null() {
            unsafe { SetEvent(event) };
        }
        // Unblock the pending ConnectNamedPipe by making a dummy connection.
        let pipe = unsafe {
            CreateFileA(
                PIPE_NAME.as_ptr(),
                GENERIC_WRITE,
                0,
                std::ptr::null(),
                OPEN_EXISTING,
                0,
                std::ptr::null_mut(),
            )
        };
        if pipe != INVALID_HANDLE_VALUE {
            unsafe { CloseHandle(pipe) };
        }
        if let Some(h) = THREAD.lock().unwrap().take() {
            let _ = h.join();
        }
        if !event.is_null() {
            unsafe { CloseHandle(event) };
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_single_instance_try_signal_existing() -> i32 {
    imp::try_signal_existing() as i32
}

/// # Safety
/// `cb` must remain callable for the lifetime of the listener thread (i.e.
/// until `jfn_single_instance_stop_listener` returns). `userdata` is opaque
/// to Rust and passed back to `cb` unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_single_instance_start_listener(
    cb: Option<unsafe extern "C" fn(*const c_char, *mut c_void)>,
    userdata: *mut c_void,
) -> i32 {
    let Some(cb) = cb else { return 0 };
    imp::start_listener(Callback { cb, userdata }) as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_single_instance_stop_listener() {
    imp::stop_listener();
}
