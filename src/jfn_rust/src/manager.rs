//! Headless app control-plane thread.
//!
//! A long-lived worker that routes app-level control work off the platform
//! main loop and off CEF's UI thread. Mirrors the playback coordinator's
//! queue + `WakeEvent` drain idiom (`jfn_playback::coordinator`), but lives in
//! the binary crate because it drives `jfn_cef` + `platform_abi` — layers
//! *above* `playback`, so it can't fold into the coordinator without a
//! dependency cycle.
//!
//! Owns the process-wide lifecycle FSM. Subsystems (X11/Wayland/macOS/Windows
//! platform layers) translate native window/power events into `ManagerMsg`
//! and post them via `jfn_manager_send`; the manager loop folds each message
//! into a single `LifecycleState` and drives the side effects (CEF visibility
//! fan-out, shutdown drain).
//!
//! The `SHUTTING_DOWN` flag (set async-signal-safely by `jfn_shutdown_initiate`)
//! carries shutdown *state* — read synchronously by the TID_UI recreate guards;
//! the queue carries the orchestration *command*. The SIGINT handler can't lock
//! the queue (interrupt context), so it wakes the manager via the signal-safe
//! bridge, and the manager — a normal-context thread — translates that wake into
//! a queued `Shutdown`.

use parking_lot::Mutex;
use std::collections::VecDeque;
use std::sync::OnceLock;
use std::thread::{self, JoinHandle};

use jfn_playback::shutdown::jfn_shutting_down;
use jfn_wake_event::WakeEvent;

/// Work routed to the manager thread. Producers post via `jfn_manager_send`
/// (platform threads) or the signal-handler bridge `jfn_manager_notify_shutdown`
/// (signal context).
pub enum ManagerMsg {
    /// Window/app became visible (true) or hidden (false). Posted by platform
    /// layers on OS-level visibility changes — Wayland xdg_toplevel
    /// suspended, X11 Map/Unmap/WM_STATE, macOS hide/unhide,
    /// Windows WM_SHOWWINDOW / SC_MINIMIZE.
    SetVisible(bool),
    /// System-level suspend / resume — power transitions (laptop lid close,
    /// macOS sleep, Windows WM_POWERBROADCAST). Treated as a stronger Hidden.
    Suspend,
    Resume,
    /// Shutdown drain. Synthesized by the manager loop when it observes the
    /// `SHUTTING_DOWN` flag — the SIGINT path can't enqueue from its handler.
    Shutdown,
}

/// Process-wide lifecycle phase. Owned by the manager loop; subsystems
/// observe transitions via the side effects the manager invokes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum LifecycleState {
    /// Foreground + visible. Default after boot.
    Running,
    /// User-visible hiding (minimize, occlusion, app hide). CEF browsers
    /// receive `WasHidden(true)`; mpv is left alone (jellyfin-web is the
    /// playback authority — see project notes).
    Hidden,
    /// System-level suspend. Same CEF posture as Hidden plus a marker so a
    /// later `Resume` always transitions back to Running regardless of any
    /// intervening visibility flap.
    Suspended,
    /// Shutdown drain in progress. Terminal.
    ShuttingDown,
}

struct Manager {
    queue: Mutex<VecDeque<ManagerMsg>>,
    wake: WakeEvent,
}

#[allow(clippy::expect_used)] // boot invariant: wake eventfd alloc is fatal if it fails
fn manager() -> &'static Manager {
    static M: OnceLock<&'static Manager> = OnceLock::new();
    M.get_or_init(|| {
        Box::leak(Box::new(Manager {
            queue: Mutex::new(VecDeque::new()),
            wake: WakeEvent::new().expect("manager WakeEvent allocation failed"),
        }))
    })
}

/// Spawn the manager thread. Long-lived; returns the join handle so the
/// teardown tail can join it once shutdown drains. Called once from
/// `run_with_cef`. Also installs the lifecycle dispatchers so platform
/// layers can post visibility / suspend / resume events without a direct
/// dep on this crate.
#[allow(clippy::expect_used)] // boot invariant: control-plane thread spawn is fatal if it fails
pub fn jfn_manager_start() -> JoinHandle<()> {
    // Materialize the singleton so its wake event exists before any producer
    // (shutdown handler / sender) signals it.
    let _ = manager();
    jfn_playback::lifecycle::jfn_lifecycle_set_handlers(
        |v| jfn_manager_send(ManagerMsg::SetVisible(v)),
        || jfn_manager_send(ManagerMsg::Suspend),
        || jfn_manager_send(ManagerMsg::Resume),
    );
    thread::Builder::new()
        .name("jfn-manager".into())
        .spawn(manager_loop)
        .expect("spawn jfn-manager thread")
}

/// Wake the manager to observe the shutdown flag. Async-signal-safe (a single
/// write to the wake event), so it's valid from the `jfn_shutdown_initiate`
/// handler in any calling context (signal handler, CEF dispatch, …).
pub fn jfn_manager_notify_shutdown() {
    manager().wake.signal();
}

/// Route work to the manager thread. Non-blocking, thread-agnostic. (No
/// callers yet — the hub seam for future control-plane work.)
pub fn jfn_manager_send(msg: ManagerMsg) {
    manager().queue.lock().push_back(msg);
    manager().wake.signal();
}

fn manager_loop() {
    let m = manager();
    let mut state = LifecycleState::Running;
    loop {
        m.wake.wait();
        m.wake.drain();

        // The signal-safe bridge wakes us with SHUTTING_DOWN set (from any
        // trigger, including the SIGINT handler that can't lock the queue).
        // Translate it into a queued message so every manager action is a
        // ManagerMsg handled in one place. Loop returns as soon as `handle`
        // observes the ShuttingDown terminal state.
        let work: VecDeque<ManagerMsg> = {
            let mut q = m.queue.lock();
            if jfn_shutting_down() && state != LifecycleState::ShuttingDown {
                q.push_back(ManagerMsg::Shutdown);
            }
            std::mem::take(&mut *q)
        };
        for msg in work {
            state = transition(state, msg);
            if state == LifecycleState::ShuttingDown {
                return;
            }
        }
    }
}

/// Apply one message to the lifecycle FSM. Returns the new state; the
/// caller observes terminal `ShuttingDown` to exit the loop. Side effects
/// happen inline (CEF visibility fan-out, shutdown drain).
fn transition(state: LifecycleState, msg: ManagerMsg) -> LifecycleState {
    use LifecycleState::*;
    match (state, msg) {
        // Shutdown is terminal and idempotent — once seen, ignore everything
        // else and don't re-enter the drain.
        (ShuttingDown, _) => ShuttingDown,
        (_, ManagerMsg::Shutdown) => {
            run_shutdown();
            ShuttingDown
        }
        // Visibility flips while running. Suspended is *not* downgraded by a
        // visibility event — the system must explicitly Resume first.
        (Running, ManagerMsg::SetVisible(false)) => {
            jfn_cef::browsers::jfn_browsers_set_hidden_all(true);
            Hidden
        }
        (Hidden, ManagerMsg::SetVisible(true)) => {
            jfn_cef::browsers::jfn_browsers_set_hidden_all(false);
            Running
        }
        (Running | Hidden, ManagerMsg::Suspend) => {
            if state == Running {
                jfn_cef::browsers::jfn_browsers_set_hidden_all(true);
            }
            Suspended
        }
        (Suspended, ManagerMsg::Resume) => {
            jfn_cef::browsers::jfn_browsers_set_hidden_all(false);
            Running
        }
        // No-op: already in the requested posture, or a stray event.
        _ => state,
    }
}

/// Orchestrate shutdown off the main thread and off TID_UI: fan out the
/// shutdown signal to every registered subsystem waker (input threads,
/// clipboard, …), then a single TID_UI task closes every browser + ships
/// the wait set back, manager blocks on `OnBeforeClose` for each, then
/// releases the process main thread to run the teardown tail. One
/// snapshot, no race between close set and wait set.
fn run_shutdown() {
    jfn_playback::shutdown::jfn_shutdown_fanout();
    jfn_cef::browsers::jfn_browsers_close_all_blocking();
    jfn_platform_abi::get().wake_main_loop();
}
