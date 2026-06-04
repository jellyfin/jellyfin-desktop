//! Single-instance gate.
//!
//! Lets a second invocation of the app using the same config directory raise
//! the existing window instead of starting a fresh process. Implementation
//! per platform:
//!
//! - Unix: AF_UNIX SOCK_STREAM socket under `$XDG_RUNTIME_DIR` or `/tmp`.
//!   The new process writes `raise [token]\n` and the running process's
//!   listener thread invokes the callback with the activation token (used
//!   by xdg-activation-v1 to focus the window when available).
//! - Windows: named pipe `\\.\pipe\jellyfin-desktop-{instanceId}`. The new
//!   process writes `raise\n` (no activation token concept on Windows).

use std::sync::OnceLock;

/// Listener callback: invoked on the listener thread when another instance
/// signals us, with the activation token (empty on Windows / when none).
type Callback = Box<dyn Fn(&str) + Send>;

static INSTANCE_ID: OnceLock<String> = OnceLock::new();

fn instance_id() -> String {
    INSTANCE_ID.get_or_init(load_or_create_instance_id).clone()
}

fn load_or_create_instance_id() -> String {
    let path = jfn_paths::config_dir().join("instance.json");
    if let Some(id) = read_instance_id(&path) {
        return id;
    }

    let id = new_instance_id();
    let value = serde_json::json!({ "instanceId": &id });
    let Ok(bytes) = serde_json::to_vec_pretty(&value) else {
        return id;
    };

    match jfn_paths::write_atomic_noclobber(&path, &bytes) {
        Ok(true) => id,
        Ok(false) => read_instance_id(&path).unwrap_or(id),
        Err(_) => read_instance_id(&path).unwrap_or(id),
    }
}

fn read_instance_id(path: &std::path::Path) -> Option<String> {
    let text = std::fs::read_to_string(path).ok()?;
    let value: serde_json::Value = serde_json::from_str(&text).ok()?;
    let id = value.get("instanceId")?.as_str()?;
    sanitize_instance_id(id)
}

fn sanitize_instance_id(id: &str) -> Option<String> {
    let clean: String = id
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
        .take(64)
        .collect();
    if clean.is_empty() { None } else { Some(clean) }
}

fn new_instance_id() -> String {
    uuid::Uuid::new_v4().simple().to_string()
}

#[cfg(unix)]
mod imp {
    use super::{Callback, instance_id};
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

    static LISTEN_FD: AtomicI32 = AtomicI32::new(-1);
    static WAKE_READ: AtomicI32 = AtomicI32::new(-1);
    static WAKE_WRITE: AtomicI32 = AtomicI32::new(-1);
    static RUNNING: AtomicBool = AtomicBool::new(false);
    static THREAD: Mutex<Option<JoinHandle<()>>> = Mutex::new(None);

    fn socket_path() -> PathBuf {
        let id = instance_id();
        let file_name = format!("jellyfin-desktop-{id}.sock");
        if let Ok(dir) = std::env::var("XDG_RUNTIME_DIR")
            && !dir.is_empty()
        {
            let mut p = PathBuf::from(dir);
            p.push(file_name);
            return p;
        }
        let uid = unsafe { getuid() };
        PathBuf::from(format!("/tmp/jellyfin-desktop-{uid}-{id}.sock"))
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
                String::from_utf8_lossy(&rest[..end]).into_owned()
            } else {
                String::new()
            };
            cb(&token);
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
        *THREAD.lock() = Some(handle);
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
        if let Some(h) = THREAD.lock().take()
            && let Err(e) = h.join()
        {
            eprintln!("[single-instance] listener thread panicked: {e:?}");
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
    use super::{Callback, instance_id};
    use parking_lot::Mutex;
    use std::ffi::CString;
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
    use std::thread::{self, JoinHandle};
    use windows_sys::Win32::Foundation::{CloseHandle, GENERIC_WRITE, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileA, OPEN_EXISTING, PIPE_ACCESS_INBOUND, ReadFile, WriteFile,
    };
    use windows_sys::Win32::System::IO::{CancelIo, OVERLAPPED};
    use windows_sys::Win32::System::Pipes::{
        ConnectNamedPipe, CreateNamedPipeA, DisconnectNamedPipe, PIPE_READMODE_BYTE,
        PIPE_TYPE_BYTE, PIPE_WAIT,
    };
    use windows_sys::Win32::System::Threading::{
        CreateEventA, INFINITE, SetEvent, WaitForMultipleObjects,
    };

    type HANDLE = *mut std::ffi::c_void;
    const WAIT_OBJECT_0: u32 = 0;
    const FILE_FLAG_OVERLAPPED: u32 = 0x40000000;

    fn pipe_name() -> CString {
        CString::new(format!(r"\\.\pipe\jellyfin-desktop-{}", instance_id()))
            .expect("sanitized instance id cannot contain NUL")
    }

    static RUNNING: AtomicBool = AtomicBool::new(false);
    static SHUTDOWN_EVENT: AtomicUsize = AtomicUsize::new(0);
    static THREAD: Mutex<Option<JoinHandle<()>>> = Mutex::new(None);

    pub fn try_signal_existing() -> bool {
        let name = pipe_name();
        let pipe = unsafe {
            CreateFileA(
                name.as_ptr().cast(),
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
            let name = pipe_name();
            let pipe = unsafe {
                CreateNamedPipeA(
                    name.as_ptr().cast(),
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
                    cb("");
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
        *THREAD.lock() = Some(handle);
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
        let name = pipe_name();
        let pipe = unsafe {
            CreateFileA(
                name.as_ptr().cast(),
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
        if let Some(h) = THREAD.lock().take()
            && let Err(e) = h.join()
        {
            eprintln!("[single-instance] listener thread panicked: {e:?}");
        }
        if !event.is_null() {
            unsafe { CloseHandle(event) };
        }
    }
}

/// Try to signal an already-running instance. Returns `true` if one was
/// reached (this process should then exit).
pub fn try_signal_existing() -> bool {
    imp::try_signal_existing()
}

/// Start the listener thread that answers future `try_signal_existing`
/// calls. `cb` runs on the listener thread with the activation token.
pub fn start_listener<F: Fn(&str) + Send + 'static>(cb: F) -> bool {
    imp::start_listener(Box::new(cb))
}

/// Stop the listener thread and release the socket / pipe.
pub fn stop_listener() {
    imp::stop_listener();
}
