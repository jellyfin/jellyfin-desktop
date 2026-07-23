//! Cross-platform one-shot event for waking poll()/WaitForMultipleObjects().
//!
//! `signal()` is async-signal-safe on POSIX (a single short `write`) so it
//! can be invoked from signal handlers.

#[cfg(target_os = "linux")]
#[path = "eventfd.rs"]
mod imp;
#[cfg(all(unix, not(target_os = "linux")))]
#[path = "pipe.rs"]
mod imp;
#[cfg(windows)]
#[path = "event.rs"]
mod imp;

#[cfg(unix)]
mod fd_wait;

pub use imp::WakeEvent;

/// Fully drain a level-triggered wake fd (eventfd or pipe read end) that was
/// signaled while unread, so a following `poll` won't immediately re-fire.
/// Reads until the fd would block. For raw fds published across threads whose
/// lifetime the caller manages; owned events use [`WakeEvent`].
#[cfg(unix)]
pub fn drain_raw_fd(fd: std::ffi::c_int) {
    let fd = unsafe { std::os::fd::BorrowedFd::borrow_raw(fd) };
    let mut buf = [0u8; 64];
    loop {
        match nix::unistd::read(fd, &mut buf) {
            Ok(0) => break,
            Ok(_) => continue,
            // Retry a signal-interrupted read; stop on would-block (drained)
            // or any real error — the wake is best-effort, not worth escalating.
            Err(nix::errno::Errno::EINTR) => continue,
            Err(_) => break,
        }
    }
}

/// Signal a wake fd created by [`WakeEvent`] (or a compatible eventfd): one
/// 8-byte write of 1. Async-signal-safe. The single home for the wake-signal
/// encoding, shared with [`WakeEvent::signal`].
#[cfg(unix)]
pub fn signal_raw_fd(fd: std::ffi::c_int) {
    let val: u64 = 1;
    let fd = unsafe { std::os::fd::BorrowedFd::borrow_raw(fd) };
    let _ = nix::unistd::write(fd, &val.to_ne_bytes());
}
