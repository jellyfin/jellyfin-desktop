//! Process-wide shutdown signal.
//!
//! A single atomic flag (`SHUTTING_DOWN`) gates teardown across the whole
//! process. Shared between SIGINT/SIGTERM, UI close, hotkeys, and CEF
//! window-close paths. `jfn_shutdown_initiate` is idempotent and
//! async-signal-safe up to whatever the registered handler does — the call
//! itself just CAS's the flag and runs the registered handler.
//!
//! A handler registered via `jfn_shutdown_set_handler` runs once on the first
//! call — it runs *inline on the calling thread*, so it MUST only signal or
//! wake (e.g. signal the shutdown manager); it must never block, close a
//! browser, or reenter CEF. The actual teardown is orchestrated off-thread by
//! the manager, which then calls `jfn_shutdown_fanout` to wake every
//! subsystem thread that registered via `jfn_shutdown_register_waker`.

use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};
use std::sync::{Mutex, PoisonError};

use crate::wake_event::WakeEvent;

static SHUTTING_DOWN: AtomicBool = AtomicBool::new(false);
static HANDLER: AtomicPtr<()> = AtomicPtr::new(std::ptr::null_mut());
// Addresses of `&'static WakeEvent`s registered for fan-out. `usize` lets a
// plain static Mutex hold them without unsafe Send/Sync gymnastics; the
// caller's `&'static` reference proves the pointer is live for the process.
static WAKERS: Mutex<Vec<usize>> = Mutex::new(Vec::new());

/// Returns true if [`jfn_shutdown_initiate`] has been called at least once.
pub fn jfn_shutting_down() -> bool {
    SHUTTING_DOWN.load(Ordering::Relaxed)
}

/// Install (or clear, with `None`) the callback invoked exactly once on the
/// first [`jfn_shutdown_initiate`] call. The callback runs inline on the
/// calling thread (possibly a signal handler or a CEF dispatch), so it MUST
/// only signal/wake — never block, close a browser, or reenter CEF.
pub fn jfn_shutdown_set_handler(handler: Option<fn()>) {
    let ptr = handler
        .map(|f| f as *mut ())
        .unwrap_or(std::ptr::null_mut());
    HANDLER.store(ptr, Ordering::Release);
}

/// Register a wake event that will be signaled when the shutdown manager
/// fans out (`jfn_shutdown_fanout`). One uniform observation pattern across
/// long-lived threads — each subsystem owns its own `WakeEvent`, polls its
/// own fd/handle alongside its native event source, and exits on signal.
///
/// `ev` must remain live for the rest of the process.
pub fn jfn_shutdown_register_waker(ev: &'static WakeEvent) {
    let addr = ev as *const WakeEvent as usize;
    let mut w = WAKERS.lock().unwrap_or_else(PoisonError::into_inner);
    w.push(addr);
}

/// Signal every registered waker. Called from the manager once it observes
/// shutdown — never from a signal handler (this locks a mutex).
pub fn jfn_shutdown_fanout() {
    let w = WAKERS.lock().unwrap_or_else(PoisonError::into_inner);
    for addr in w.iter() {
        let ev = *addr as *const WakeEvent;
        unsafe { crate::wake_event::jfn_wake_event_signal(ev) };
    }
}

/// Idempotent. First call: sets the flag and runs the registered handler.
/// Subsequent calls are no-ops. Async-signal-safe up to whatever the
/// handler does.
pub fn jfn_shutdown_initiate() {
    if SHUTTING_DOWN
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Relaxed)
        .is_err()
    {
        return;
    }
    let h = HANDLER.load(Ordering::Acquire);
    if !h.is_null() {
        let f: fn() = unsafe { std::mem::transmute(h) };
        f();
    }
}
