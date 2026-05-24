//! Save/restore SIGINT and SIGTERM dispositions around a custom handler.
//!
//! Snapshot prior `sigaction`s on `install`; restore them on `Drop`.

#![cfg(unix)]

use libc::{SIGINT, SIGTERM, c_int, sigaction, sigemptyset};

pub struct SignalGuard {
    prev_int: sigaction,
    prev_term: sigaction,
}

impl Drop for SignalGuard {
    fn drop(&mut self) {
        unsafe {
            libc::sigaction(SIGINT, &self.prev_int, std::ptr::null_mut());
            libc::sigaction(SIGTERM, &self.prev_term, std::ptr::null_mut());
        }
    }
}

/// # Safety
/// `handler` must be async-signal-safe: it runs from inside a `sigaction`
/// handler installed on SIGINT/SIGTERM.
pub unsafe fn install(handler: unsafe extern "C" fn(c_int)) -> SignalGuard {
    let mut sa: sigaction = unsafe { std::mem::zeroed() };
    sa.sa_sigaction = handler as usize;
    unsafe { sigemptyset(&mut sa.sa_mask) };

    let mut prev_int: sigaction = unsafe { std::mem::zeroed() };
    let mut prev_term: sigaction = unsafe { std::mem::zeroed() };
    unsafe {
        libc::sigaction(SIGINT, &sa, &mut prev_int);
        libc::sigaction(SIGTERM, &sa, &mut prev_term);
    }
    SignalGuard {
        prev_int,
        prev_term,
    }
}
