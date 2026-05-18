//! Save/restore SIGINT and SIGTERM dispositions around a custom handler.
//!
//! Mirrors the C++ `SignalHandlerGuard` RAII pattern that wraps
//! `CefInitialize`: snapshot prior `sigaction`s, install the supplied
//! handler, restore on drop.

#![cfg(unix)]

use libc::{SIGINT, SIGTERM, c_int, sigaction, sigemptyset};

pub struct SignalGuard {
    prev_int: sigaction,
    prev_term: sigaction,
}

/// # Safety
/// `handler` must be an async-signal-safe function: it will run inside a
/// `sigaction` handler installed on SIGINT/SIGTERM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_signal_guard_install(
    handler: Option<unsafe extern "C" fn(c_int)>,
) -> *mut SignalGuard {
    let Some(handler) = handler else {
        return std::ptr::null_mut();
    };

    let mut sa: sigaction = unsafe { std::mem::zeroed() };
    sa.sa_sigaction = handler as usize;
    unsafe { sigemptyset(&mut sa.sa_mask) };

    let mut prev_int: sigaction = unsafe { std::mem::zeroed() };
    let mut prev_term: sigaction = unsafe { std::mem::zeroed() };
    unsafe {
        libc::sigaction(SIGINT, &sa, &mut prev_int);
        libc::sigaction(SIGTERM, &sa, &mut prev_term);
    }
    Box::into_raw(Box::new(SignalGuard {
        prev_int,
        prev_term,
    }))
}

/// # Safety
/// `guard` must be null or a pointer previously returned by
/// [`jfn_signal_guard_install`]; each pointer may only be freed once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_signal_guard_free(guard: *mut SignalGuard) {
    if guard.is_null() {
        return;
    }
    let boxed = unsafe { Box::from_raw(guard) };
    unsafe {
        libc::sigaction(SIGINT, &boxed.prev_int, std::ptr::null_mut());
        libc::sigaction(SIGTERM, &boxed.prev_term, std::ptr::null_mut());
    }
}
