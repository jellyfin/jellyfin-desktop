use std::ffi::c_int;
use std::os::fd::AsRawFd;

use nix::sys::eventfd::{EfdFlags, EventFd};

pub struct WakeEvent {
    fd: EventFd,
}

impl WakeEvent {
    pub fn new() -> Option<Self> {
        let fd = EventFd::from_value_and_flags(0, EfdFlags::EFD_NONBLOCK | EfdFlags::EFD_CLOEXEC)
            .ok()?;
        Some(WakeEvent { fd })
    }

    pub fn fd(&self) -> c_int {
        self.fd.as_raw_fd()
    }

    pub fn signal(&self) {
        crate::signal_raw_fd(self.fd.as_raw_fd());
    }

    pub fn drain(&self) {
        let _ = self.fd.read();
    }

    /// Block until signaled. Level-triggered, so a `signal()` that lands
    /// before the call returns immediately.
    pub fn wait(&self) {
        crate::fd_wait::wait(self.fd.as_raw_fd());
    }
}
