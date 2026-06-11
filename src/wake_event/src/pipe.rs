use libc::{c_int, c_void};

pub struct WakeEvent {
    read_fd: c_int,
    write_fd: c_int,
}

impl WakeEvent {
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

    pub fn fd(&self) -> c_int {
        self.read_fd
    }

    pub fn signal(&self) {
        let byte: u8 = 1;
        unsafe {
            libc::write(self.write_fd, &byte as *const u8 as *const c_void, 1);
        }
    }

    pub fn drain(&self) {
        let mut buf = [0u8; 64];
        loop {
            let n = unsafe { libc::read(self.read_fd, buf.as_mut_ptr() as *mut c_void, buf.len()) };
            if n <= 0 {
                break;
            }
        }
    }

    /// Block until signaled. Level-triggered, so a `signal()` that lands
    /// before the call returns immediately.
    pub fn wait(&self) {
        crate::fd_wait::wait(self.read_fd);
    }
}

impl Drop for WakeEvent {
    fn drop(&mut self) {
        if self.read_fd >= 0 {
            unsafe { libc::close(self.read_fd) };
        }
        if self.write_fd >= 0 {
            unsafe { libc::close(self.write_fd) };
        }
    }
}
