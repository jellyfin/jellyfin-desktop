//! Process-wide shutdown signal.
//!
//! A single atomic flag (`SHUTTING_DOWN`) gates teardown across the whole
//! process. Shared between SIGINT/SIGTERM, UI close, hotkeys, and CEF
//! window-close paths. `jfn_shutdown_initiate` is idempotent and
//! async-signal-safe up to whatever the registered handler does — the call
//! itself just CAS's the flag and signals the wake event.
//!
//! A wake event lives alongside the flag so threads parked in `poll()` or
//! `WaitForMultipleObjects` (e.g. the X11 input thread) can be unblocked. A
//! handler registered via `jfn_shutdown_set_handler` runs once on the first
//! call — it runs *inline on the calling thread*, so it MUST only signal or
//! wake (e.g. signal the shutdown manager); it must never block, close a
//! browser, or reenter CEF. The actual teardown is orchestrated off-thread by
//! the manager.

use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};

use crate::wake_event::WakeEvent;

static SHUTTING_DOWN: AtomicBool = AtomicBool::new(false);
static HANDLER: AtomicPtr<()> = AtomicPtr::new(std::ptr::null_mut());

fn wake_event() -> &'static WakeEvent {
    use std::sync::OnceLock;
    static EV: OnceLock<&'static WakeEvent> = OnceLock::new();
    EV.get_or_init(|| {
        let raw = WakeEvent::new().expect("shutdown WakeEvent allocation failed");
        Box::leak(Box::new(raw))
    })
}

/// Returns true if [`jfn_shutdown_initiate`] has been called at least once.
pub fn jfn_shutting_down() -> bool {
    SHUTTING_DOWN.load(Ordering::Relaxed)
}

/// Pointer to the process-wide shutdown wake event. The returned pointer is
/// valid for the remainder of the process.
pub fn jfn_shutdown_event() -> *const WakeEvent {
    wake_event() as *const _
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

/// Idempotent. First call: sets the flag, signals the wake event, runs the
/// registered handler. Subsequent calls are no-ops.
pub fn jfn_shutdown_initiate() {
    if SHUTTING_DOWN
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Relaxed)
        .is_err()
    {
        return;
    }
    unsafe { crate::wake_event::jfn_wake_event_signal(wake_event()) };
    let h = HANDLER.load(Ordering::Acquire);
    if !h.is_null() {
        let f: fn() = unsafe { std::mem::transmute(h) };
        f();
    }
}
