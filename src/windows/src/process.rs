//! Windows overrides for the `Platform` process-lifecycle methods: console
//! ctrl shutdown handler and the named-pipe single-instance gate.

use std::sync::OnceLock;

// =====================================================================
// Shutdown signal (console ctrl handler)
// =====================================================================

static SHUTDOWN_CB: OnceLock<fn()> = OnceLock::new();

unsafe extern "system" fn console_ctrl_handler(_t: u32) -> i32 {
    if let Some(cb) = SHUTDOWN_CB.get() {
        cb();
    }
    1
}

pub fn install_shutdown(on_shutdown: fn()) {
    let _ = SHUTDOWN_CB.set(on_shutdown);
    unsafe extern "system" {
        fn SetConsoleCtrlHandler(handler: unsafe extern "system" fn(u32) -> i32, add: i32) -> i32;
    }
    unsafe { SetConsoleCtrlHandler(console_ctrl_handler, 1) };
}

// =====================================================================
// Single-instance gate (named pipe)
// =====================================================================

mod single_instance {
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

    use jfn_platform_abi::Callback;

    type Handle = *mut std::ffi::c_void;
    const WAIT_OBJECT_0: u32 = 0;
    const FILE_FLAG_OVERLAPPED: u32 = 0x40000000;

    fn pipe_name(instance_id: &str) -> CString {
        CString::new(format!(r"\\.\pipe\jellyfin-desktop-{instance_id}")).unwrap_or_default()
    }

    static RUNNING: AtomicBool = AtomicBool::new(false);
    static SHUTDOWN_EVENT: AtomicUsize = AtomicUsize::new(0);
    static THREAD: Mutex<Option<JoinHandle<()>>> = Mutex::new(None);

    pub fn try_signal_existing(instance_id: &str) -> bool {
        let name = pipe_name(instance_id);
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

    fn listener_loop(instance_id: String, cb: Callback) {
        let shutdown = SHUTDOWN_EVENT.load(Ordering::Acquire) as Handle;
        while RUNNING.load(Ordering::Acquire) {
            let name = pipe_name(&instance_id);
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

    pub fn start_listener(instance_id: &str, cb: Callback) -> bool {
        if RUNNING.load(Ordering::Acquire) {
            return true;
        }
        let event = unsafe { CreateEventA(std::ptr::null(), 1, 0, std::ptr::null()) };
        if event.is_null() {
            return false;
        }
        SHUTDOWN_EVENT.store(event as usize, Ordering::Release);
        RUNNING.store(true, Ordering::Release);
        let id = instance_id.to_owned();
        let handle = thread::spawn(move || listener_loop(id, cb));
        *THREAD.lock() = Some(handle);
        true
    }

    pub fn stop_listener(instance_id: &str) {
        if !RUNNING.swap(false, Ordering::AcqRel) {
            return;
        }
        let event = SHUTDOWN_EVENT.swap(0, Ordering::AcqRel) as Handle;
        if !event.is_null() {
            unsafe { SetEvent(event) };
        }
        // Unblock the pending ConnectNamedPipe by making a dummy connection.
        let name = pipe_name(instance_id);
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

pub use single_instance::{start_listener, stop_listener, try_signal_existing};
