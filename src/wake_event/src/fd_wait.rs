use libc::c_int;

/// Block until `fd` is readable. Level-triggered, so a signal that lands
/// before the call returns immediately. Returns on any non-`EINTR`
/// `poll` error rather than spinning.
pub fn wait(fd: c_int) {
    let mut pfd = libc::pollfd {
        fd,
        events: libc::POLLIN,
        revents: 0,
    };
    loop {
        let r = unsafe { libc::poll(&mut pfd as *mut libc::pollfd, 1, -1) };
        if r < 0 {
            if std::io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            return;
        }
        if pfd.revents != 0 {
            return;
        }
    }
}
