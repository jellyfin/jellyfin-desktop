use std::ffi::c_int;
use std::os::fd::{AsRawFd, OwnedFd};

use nix::fcntl::{FcntlArg, FdFlag, OFlag, fcntl};
use nix::unistd::{pipe, read, write};

pub struct WakeEvent {
    read_fd: OwnedFd,
    write_fd: OwnedFd,
}

impl WakeEvent {
    pub fn new() -> Option<Self> {
        let (read_fd, write_fd) = pipe().ok()?;
        for f in [&read_fd, &write_fd] {
            let flags = fcntl(f, FcntlArg::F_GETFL).ok()?;
            let flags = OFlag::from_bits_retain(flags) | OFlag::O_NONBLOCK;
            fcntl(f, FcntlArg::F_SETFL(flags)).ok()?;
            fcntl(f, FcntlArg::F_SETFD(FdFlag::FD_CLOEXEC)).ok()?;
        }
        Some(WakeEvent { read_fd, write_fd })
    }

    pub fn fd(&self) -> c_int {
        self.read_fd.as_raw_fd()
    }

    pub fn signal(&self) {
        let _ = write(&self.write_fd, &[1u8]);
    }

    pub fn drain(&self) {
        let mut buf = [0u8; 64];
        while let Ok(n) = read(&self.read_fd, &mut buf) {
            if n == 0 {
                break;
            }
        }
    }

    /// Block until signaled. Level-triggered, so a `signal()` that lands
    /// before the call returns immediately.
    pub fn wait(&self) {
        crate::fd_wait::wait(self.read_fd.as_raw_fd());
    }
}
