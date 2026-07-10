//! How the platform hosts mpv: pre-create environment, host-window
//! readiness, the VO wait loop, and severing host links at teardown.
//!
//! No mpv types appear here — shared code owns all mpv event handling via
//! the `pump` closure, and the platform owns only the wait strategy.

use crate::WindowDecorations;

/// Platform side of mpv's lifecycle. Defaults cover backends where mpv
/// needs no host preparation and the generic blocking wait suffices.
pub trait MpvHost: Send + Sync {
    /// Prepare the process environment for mpv. Runs before `mpv_create`;
    /// position-critical setup (window-ownership proxies, env vars mpv
    /// reads during init) belongs here.
    fn prepare(&self, _decorations: WindowDecorations) {}

    /// Whether the host window state mpv's VO depends on (scale, first
    /// configure) is known. Gates VO-startup completion — not VO state
    /// itself, which mpv owns.
    fn host_ready(&self) -> bool {
        true
    }

    /// The host's authoritative maximized state, or `None` when mpv — not the
    /// host — owns it; core then resolves `None` against mpv's `window-maximized`
    /// property. A host that owns its own toplevel (Wayland) returns `Some`.
    fn window_maximized(&self) -> Option<bool> {
        None
    }

    fn ensure_host_window(&self) {}

    /// Own the VO wait loop. `pump(may_block)` drains queued mpv events
    /// (and, when `may_block`, may additionally wait for the next one);
    /// it returns `false` once waiting is over. Platforms that must keep
    /// a native run loop serviced call `pump(false)` and block on their
    /// own loop between calls.
    fn run_vo_wait(&self, pump: &mut dyn FnMut(bool) -> bool) {
        while pump(true) {}
    }

    /// Logical content size of the host window in points, when the OS —
    /// not mpv's osd-dimensions — is the authority for it.
    fn logical_content_size(&self) -> Option<(i32, i32)> {
        None
    }

    /// Sever host↔mpv links that could deadlock teardown. Called
    /// immediately before CEF teardown.
    fn detach(&self) {}
}

/// All-default host for backends where mpv needs nothing from the
/// platform (X11: mpv owns its window outright).
pub struct DefaultMpvHost;

impl MpvHost for DefaultMpvHost {}
