//! Cross-platform one-shot event for waking poll()/WaitForMultipleObjects().
//!
//! - Linux:   `eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)`.
//! - macOS:   pipe with both ends `O_NONBLOCK | FD_CLOEXEC`.
//! - Windows: manual-reset event from `CreateEventW`.
//!
//! `signal()` is async-signal-safe on POSIX (a single short `write`) so it
//! can be invoked from signal handlers.

#[cfg(unix)]
mod imp {
    use libc::{c_int, c_void};

    pub struct WakeEvent {
        #[cfg(target_os = "linux")]
        fd: c_int,
        #[cfg(not(target_os = "linux"))]
        read_fd: c_int,
        #[cfg(not(target_os = "linux"))]
        write_fd: c_int,
    }

    impl WakeEvent {
        #[cfg(target_os = "linux")]
        pub fn new() -> Option<Self> {
            let fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
            if fd < 0 {
                return None;
            }
            Some(WakeEvent { fd })
        }

        #[cfg(not(target_os = "linux"))]
        pub fn new() -> Option<Self> {
            let mut fds: [c_int; 2] = [-1, -1];
            if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 {
                return None;
            }
            for f in &fds {
                unsafe {
                    let flags = libc::fcntl(*f, libc::F_GETFL);
                    libc::fcntl(*f, libc::F_SETFL, flags | libc::O_NONBLOCK);
                    libc::fcntl(*f, libc::F_SETFD, libc::FD_CLOEXEC);
                }
            }
            Some(WakeEvent {
                read_fd: fds[0],
                write_fd: fds[1],
            })
        }

        #[cfg(target_os = "linux")]
        pub fn fd(&self) -> c_int {
            self.fd
        }

        #[cfg(not(target_os = "linux"))]
        pub fn fd(&self) -> c_int {
            self.read_fd
        }

        #[cfg(target_os = "linux")]
        pub fn signal(&self) {
            let val: u64 = 1;
            unsafe {
                libc::write(
                    self.fd,
                    &val as *const u64 as *const c_void,
                    core::mem::size_of::<u64>(),
                );
            }
        }

        #[cfg(not(target_os = "linux"))]
        pub fn signal(&self) {
            let byte: u8 = 1;
            unsafe {
                libc::write(self.write_fd, &byte as *const u8 as *const c_void, 1);
            }
        }

        #[cfg(target_os = "linux")]
        pub fn drain(&self) {
            let mut val: u64 = 0;
            unsafe {
                libc::read(
                    self.fd,
                    &mut val as *mut u64 as *mut c_void,
                    core::mem::size_of::<u64>(),
                );
            }
        }

        #[cfg(not(target_os = "linux"))]
        pub fn drain(&self) {
            let mut buf = [0u8; 64];
            loop {
                let n =
                    unsafe { libc::read(self.read_fd, buf.as_mut_ptr() as *mut c_void, buf.len()) };
                if n <= 0 {
                    break;
                }
            }
        }
    }

    impl Drop for WakeEvent {
        #[cfg(target_os = "linux")]
        fn drop(&mut self) {
            if self.fd >= 0 {
                unsafe { libc::close(self.fd) };
            }
        }

        #[cfg(not(target_os = "linux"))]
        fn drop(&mut self) {
            if self.read_fd >= 0 {
                unsafe { libc::close(self.read_fd) };
            }
            if self.write_fd >= 0 {
                unsafe { libc::close(self.write_fd) };
            }
        }
    }
}

#[cfg(windows)]
mod imp {
    use core::ptr;
    use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
    use windows_sys::Win32::System::Threading::{CreateEventW, ResetEvent, SetEvent};

    pub struct WakeEvent {
        handle: HANDLE,
    }

    impl WakeEvent {
        pub fn new() -> Option<Self> {
            // manual-reset, initially non-signaled
            let h = unsafe { CreateEventW(ptr::null(), 1, 0, ptr::null()) };
            if h.is_null() {
                return None;
            }
            Some(WakeEvent { handle: h })
        }

        pub fn handle(&self) -> HANDLE {
            self.handle
        }

        pub fn signal(&self) {
            unsafe {
                SetEvent(self.handle);
            }
        }

        pub fn drain(&self) {
            unsafe {
                ResetEvent(self.handle);
            }
        }
    }

    impl Drop for WakeEvent {
        fn drop(&mut self) {
            if !self.handle.is_null() {
                unsafe { CloseHandle(self.handle) };
            }
        }
    }
}

pub use imp::WakeEvent;

#[unsafe(no_mangle)]
pub extern "C" fn jfn_wake_event_new() -> *mut WakeEvent {
    match WakeEvent::new() {
        Some(ev) => Box::into_raw(Box::new(ev)),
        None => core::ptr::null_mut(),
    }
}

/// Free a wake event.
///
/// # Safety
/// `ev` must be a pointer previously returned by `jfn_wake_event_new`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_wake_event_free(ev: *mut WakeEvent) {
    if !ev.is_null() {
        unsafe { drop(Box::from_raw(ev)) };
    }
}

/// Wake any waiter on the event. Async-signal-safe on POSIX.
///
/// # Safety
/// `ev` must point to a live wake event or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_wake_event_signal(ev: *const WakeEvent) {
    if let Some(ev) = unsafe { ev.as_ref() } {
        ev.signal();
    }
}

/// Consume pending signals so the next wait blocks.
///
/// # Safety
/// `ev` must point to a live wake event or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_wake_event_drain(ev: *const WakeEvent) {
    if let Some(ev) = unsafe { ev.as_ref() } {
        ev.drain();
    }
}

#[cfg(unix)]
/// Readable fd for poll().
///
/// # Safety
/// `ev` must point to a live wake event or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_wake_event_fd(ev: *const WakeEvent) -> libc::c_int {
    match unsafe { ev.as_ref() } {
        Some(ev) => ev.fd(),
        None => -1,
    }
}

#[cfg(windows)]
/// HANDLE for WaitForMultipleObjects.
///
/// # Safety
/// `ev` must point to a live wake event or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_wake_event_handle(ev: *const WakeEvent) -> *mut core::ffi::c_void {
    match unsafe { ev.as_ref() } {
        Some(ev) => ev.handle() as *mut core::ffi::c_void,
        None => core::ptr::null_mut(),
    }
}
