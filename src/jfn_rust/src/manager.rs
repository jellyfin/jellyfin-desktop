//! Headless app control-plane thread.
//!
//! A long-lived worker that routes app-level control work off the platform
//! main loop and off CEF's UI thread. Mirrors the playback coordinator's
//! queue + `WakeEvent` drain idiom (`jfn_playback::coordinator`), but lives in
//! the binary crate because it drives `jfn_cef` + `platform_abi` — layers
//! *above* `playback`, so it can't fold into the coordinator without a
//! dependency cycle.
//!
//! Today its only client is shutdown, modeled as a `ManagerMsg::Shutdown`. The
//! `SHUTTING_DOWN` flag (set async-signal-safely by `jfn_shutdown_initiate`)
//! carries shutdown *state* — read synchronously by the TID_UI recreate guards;
//! the queue carries the orchestration *command*. The SIGINT handler can't lock
//! the queue (interrupt context), so it wakes the manager via the signal-safe
//! bridge, and the manager — a normal-context thread — translates that wake into
//! a queued `Shutdown`. Every manager action is therefore one `ManagerMsg`
//! dispatched in one place. `jfn_manager_send` is the seam for future
//! non-shutdown producers, who enqueue without blocking the platform/CEF loops.

use std::collections::VecDeque;
use std::sync::{Mutex, OnceLock};
use std::thread::{self, JoinHandle};

use jfn_playback::WakeEvent;
use jfn_playback::shutdown::jfn_shutting_down;

/// Work routed to the manager thread. `Shutdown` is synthesized by the manager
/// loop when it observes the shutdown flag (the SIGINT path can't enqueue from
/// its handler); future non-shutdown clients add variants here and post them
/// via `jfn_manager_send`.
pub enum ManagerMsg {
    Shutdown,
}

struct Manager {
    queue: Mutex<VecDeque<ManagerMsg>>,
    wake: WakeEvent,
}

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
/// `run_with_cef`.
pub fn jfn_manager_start() -> JoinHandle<()> {
    // Materialize the singleton so its wake event exists before any producer
    // (shutdown handler / sender) signals it.
    let _ = manager();
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
    manager().queue.lock().unwrap().push_back(msg);
    manager().wake.signal();
}

fn manager_loop() {
    let m = manager();
    loop {
        m.wake.wait();
        m.wake.drain();

        // The signal-safe bridge wakes us with SHUTTING_DOWN set (from any
        // trigger, including the SIGINT handler that can't lock the queue).
        // Translate it into a queued message so every manager action is a
        // ManagerMsg handled in one place. Runs at most once: the loop returns
        // as soon as `handle` reports the shutdown was processed.
        let work: VecDeque<ManagerMsg> = {
            let mut q = m.queue.lock().unwrap();
            if jfn_shutting_down() {
                q.push_back(ManagerMsg::Shutdown);
            }
            std::mem::take(&mut *q)
        };
        for msg in work {
            if handle(msg) {
                return;
            }
        }
    }
}

/// Dispatch one message. Returns `true` if it terminates the manager (shutdown).
fn handle(msg: ManagerMsg) -> bool {
    match msg {
        ManagerMsg::Shutdown => {
            run_shutdown();
            true
        }
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
