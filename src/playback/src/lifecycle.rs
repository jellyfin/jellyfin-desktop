//! Process-wide lifecycle event seam.
//!
//! Platform crates (X11, Wayland, macOS, Windows) translate native window
//! and power events into the three calls in this module; the binary
//! crate's manager installs handlers that fold each into its lifecycle
//! FSM. Function-pointer indirection because platform crates live below
//! the manager in the dependency tree — same pattern as
//! [`jfn_shutdown_set_handler`](crate::shutdown::jfn_shutdown_set_handler).
//!
//! Calls are best-effort: if no handler is installed yet (e.g. during
//! boot), the event is dropped. None of the producers block on delivery.

use std::sync::atomic::{AtomicPtr, Ordering};

static SET_VISIBLE: AtomicPtr<()> = AtomicPtr::new(std::ptr::null_mut());
static SUSPEND: AtomicPtr<()> = AtomicPtr::new(std::ptr::null_mut());
static RESUME: AtomicPtr<()> = AtomicPtr::new(std::ptr::null_mut());

/// Install the three lifecycle dispatchers. Passing the same `fn` is fine;
/// each slot is independent. Replaces any previously installed handlers.
pub fn jfn_lifecycle_set_handlers(visible: fn(bool), suspend: fn(), resume: fn()) {
    SET_VISIBLE.store(visible as *mut (), Ordering::Release);
    SUSPEND.store(suspend as *mut (), Ordering::Release);
    RESUME.store(resume as *mut (), Ordering::Release);
}

/// Report a window/app visibility transition. `visible=false` covers
/// minimize, occlusion, app-hide; `true` covers restore and unhide.
pub fn jfn_lifecycle_set_visible(visible: bool) {
    let p = SET_VISIBLE.load(Ordering::Acquire);
    if !p.is_null() {
        let f: fn(bool) = unsafe { std::mem::transmute(p) };
        f(visible);
    }
}

/// Report a system-level suspend (laptop lid close, macOS sleep, Windows
/// APM suspend). Stronger than `set_visible(false)` — pairs with a later
/// [`jfn_lifecycle_resume`] when the system wakes.
pub fn jfn_lifecycle_suspend() {
    let p = SUSPEND.load(Ordering::Acquire);
    if !p.is_null() {
        let f: fn() = unsafe { std::mem::transmute(p) };
        f();
    }
}

/// Report a system-level resume — the inverse of [`jfn_lifecycle_suspend`].
pub fn jfn_lifecycle_resume() {
    let p = RESUME.load(Ordering::Acquire);
    if !p.is_null() {
        let f: fn() = unsafe { std::mem::transmute(p) };
        f();
    }
}
