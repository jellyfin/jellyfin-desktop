use libc::{c_int, c_void};

pub struct WakeEvent {
    fd: c_int,
}

impl WakeEvent {
    pub fn new() -> Option<Self> {
        let fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
        if fd < 0 {
            return None;
        }
        Some(WakeEvent { fd })
    }

    pub fn fd(&self) -> c_int {
        self.fd
    }

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

    /// Block until signaled. Level-triggered, so a `signal()` that lands
    /// before the call returns immediately.
    pub fn wait(&self) {
        crate::fd_wait::wait(self.fd);
    }
}

impl Drop for WakeEvent {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe { libc::close(self.fd) };
        }
    }
}
