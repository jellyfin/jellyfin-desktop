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
