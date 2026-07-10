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
    let mut buf = [0u8; 64];
    loop {
        let n = unsafe { libc::read(fd, buf.as_mut_ptr().cast(), buf.len()) };
        if n > 0 {
            continue;
        }
        if n == 0 {
            break;
        }
        // n < 0: retry a signal-interrupted read; stop on would-block (drained)
        // or any real error — the wake is best-effort, not worth escalating.
        if std::io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) {
            continue;
        }
        break;
    }
}

/// Signal a wake fd created by [`WakeEvent`] (or a compatible eventfd): one
/// 8-byte write of 1. Async-signal-safe. The single home for the wake-signal
/// encoding, shared with [`WakeEvent::signal`].
#[cfg(unix)]
pub fn signal_raw_fd(fd: std::ffi::c_int) {
    let val: u64 = 1;
    unsafe {
        libc::write(fd, (&raw const val).cast(), core::mem::size_of::<u64>());
    }
}
