//! Surface alpha-fade animation thread.
//!
//! Owns the join-handle and stop flag for the at-most-one fade animation
//! in flight. The per-frame protocol calls
//! (`wp_alpha_modifier_surface_v1_set_multiplier` + commit +
//! display_flush) live in `wl_ops` — this module just drives the loop on
//! a dedicated thread, gates each iteration on the stop flag, and fires
//! the caller-supplied start/complete closures.

use std::ffi::c_void;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::{self, JoinHandle};
use std::time::Duration;

/// Per-frame apply callback. Returns false to abort the loop (surface
/// gone, alpha modifier dropped, etc.).
type ApplyFrameFn = unsafe extern "C" fn(surface: *mut c_void, alpha: u32) -> bool;

static STOP: AtomicBool = AtomicBool::new(false);
static HANDLE: Mutex<Option<JoinHandle<()>>> = Mutex::new(None);

fn join_existing() {
    let handle = HANDLE.lock().unwrap().take();
    if let Some(h) = handle {
        STOP.store(true, Ordering::Release);
        let _ = h.join();
    }
}

/// Start a fade. Returns false when the caller should skip the animation
/// entirely (surface unfadable, zero frame budget) — in that case the
/// closures are dropped without firing.
///
/// On success the spawned thread will:
///   1. fire `on_start` once (before the loop)
///   2. loop `total_frames` times, calling `apply` with the current
///      alpha; bail out early if `apply` returns false or another fade
///      preempts via `jfn_wl_fade_stop_all`
///   3. fire `on_done` once on natural completion (skipped on abort)
///
/// Safety: `surface` must remain valid until the fade ends or
/// `jfn_wl_fade_stop_all` is called.
pub unsafe fn jfn_wl_fade_start(
    surface: *mut c_void,
    fade_sec: f32,
    fps: f64,
    apply: ApplyFrameFn,
    on_start: Option<Box<dyn FnOnce() + Send>>,
    on_done: Option<Box<dyn FnOnce() + Send>>,
) -> bool {
    if surface.is_null() || fps <= 0.0 {
        return false;
    }

    join_existing();
    STOP.store(false, Ordering::Release);

    let surface_addr = surface as usize;

    let handle = thread::spawn(move || {
        if let Some(f) = on_start {
            f();
        }

        let mut total_frames = (fade_sec as f64 * fps) as i32;
        if total_frames < 1 {
            total_frames = 1;
        }
        let frame_duration = Duration::from_micros((1e6 / fps) as u64);

        let mut aborted = false;
        for i in 1..=total_frames {
            if STOP.load(Ordering::Acquire) {
                aborted = true;
                break;
            }
            let t = i as f32 / total_frames as f32;
            let alpha = ((1.0 - t) * u32::MAX as f32) as u32;
            let ok = unsafe { apply(surface_addr as *mut c_void, alpha) };
            if !ok {
                break;
            }
            thread::sleep(frame_duration);
        }

        if aborted {
            // Drop on_done without firing — suppress on_complete when
            // stop_fade_thread() preempts.
            return;
        }
        if let Some(f) = on_done {
            f();
        }
    });

    *HANDLE.lock().unwrap() = Some(handle);
    true
}

/// Stop the in-flight fade (if any) and join its thread.
pub fn jfn_wl_fade_stop_all() {
    join_existing();
}
