//! Surface alpha-fade animation thread.
//!
//! Owns the join-handle and stop flag for the at-most-one fade animation in
//! flight. The per-frame protocol calls
//! (`wp_alpha_modifier_surface_v1_set_multiplier` + commit + display_flush)
//! live on the C++ side — this module just drives the loop on a dedicated
//! thread, gates each iteration on the stop flag, and fires the
//! caller-supplied start/complete callbacks.
//!
//! The C++ vtable thunk for `fade_surface` calls `jfn_wl_fade_start`. The
//! cleanup path calls `jfn_wl_fade_stop_all` before tearing down surfaces
//! the in-flight fade may still touch.

use std::ffi::c_void;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::{self, JoinHandle};
use std::time::Duration;

/// Per-frame apply callback. Returns false to abort the loop (surface gone,
/// alpha modifier dropped, etc.). Called under the C++ `surface_mtx` —
/// implementation is in wayland.cpp.
type ApplyFrameFn = unsafe extern "C" fn(surface: *mut c_void, alpha: u32) -> bool;

type SimpleCb = unsafe extern "C" fn(ctx: *mut c_void);
type CtxDtor = unsafe extern "C" fn(ctx: *mut c_void);

struct CbTriple {
    cb: Option<SimpleCb>,
    ctx: *mut c_void,
    dtor: Option<CtxDtor>,
}

unsafe impl Send for CbTriple {}

impl CbTriple {
    fn new(cb: Option<SimpleCb>, ctx: *mut c_void, dtor: Option<CtxDtor>) -> Self {
        Self { cb, ctx, dtor }
    }

    fn fire(&self) {
        if let Some(f) = self.cb {
            unsafe { f(self.ctx) };
        }
    }
}

impl Drop for CbTriple {
    fn drop(&mut self) {
        if let Some(d) = self.dtor {
            unsafe { d(self.ctx) };
        }
    }
}

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
/// caller must fire its own start + complete callbacks; this function
/// consumed neither.
///
/// On success the returned thread will:
///   1. fire `on_start` once (before the loop)
///   2. loop `total_frames` times, calling `apply` with the current alpha;
///      bail out early if `apply` returns false or another fade is started
///   3. fire `on_done` once on natural completion (skipped on abort)
///
/// Safety: `surface` must remain valid until the fade ends or
/// `jfn_wl_fade_stop_all` is called. Apply / start / done callbacks are
/// invoked from the fade thread.
pub unsafe fn jfn_wl_fade_start(
    surface: *mut c_void,
    fade_sec: f32,
    fps: f64,
    apply: ApplyFrameFn,
    on_start: Option<SimpleCb>,
    start_ctx: *mut c_void,
    start_dtor: Option<CtxDtor>,
    on_done: Option<SimpleCb>,
    done_ctx: *mut c_void,
    done_dtor: Option<CtxDtor>,
) -> bool {
    if surface.is_null() || fps <= 0.0 {
        // Caller fires its own callbacks; drop the triples we received.
        drop(CbTriple::new(on_start, start_ctx, start_dtor));
        drop(CbTriple::new(on_done, done_ctx, done_dtor));
        return false;
    }

    join_existing();
    STOP.store(false, Ordering::Release);

    let start_triple = CbTriple::new(on_start, start_ctx, start_dtor);
    let done_triple = CbTriple::new(on_done, done_ctx, done_dtor);
    let surface_addr = surface as usize;

    let handle = thread::spawn(move || {
        start_triple.fire();

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
            // Drop done_triple without firing — matches C++ behaviour where
            // on_complete is suppressed when stop_fade_thread() preempts.
            return;
        }
        done_triple.fire();
    });

    *HANDLE.lock().unwrap() = Some(handle);
    true
}

/// Stop the in-flight fade (if any) and join its thread. Called from C++
/// cleanup before destroying the alpha-modifier proxy and the surfaces.
pub fn jfn_wl_fade_stop_all() {
    join_existing();
}
