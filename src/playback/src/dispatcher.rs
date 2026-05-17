//! mpv → playback coordinator bridge.
//!
//! Owns a queue of `MpvEvent`s and a consumer thread that drains the
//! queue (routing each event into the playback coordinator) and pumps
//! the C++ sink wake fds it was given so the per-sink queues
//! (BrowserPlaybackSink, MpvActionSink, …) flush onto the cef thread.
//!
//! Lifecycle:
//!   `jfn_dispatcher_init`
//!   `jfn_dispatcher_register_sink_pump(ctx, fd, pump)` for each sink
//!   `jfn_dispatcher_set_display_scale_handler(fn)` (browsers setScale)
//!   `jfn_dispatcher_start`
//!   ... `jfn_dispatcher_publish` from any thread ...
//!   `jfn_dispatcher_stop` then `jfn_dispatcher_shutdown`

use std::collections::VecDeque;
use std::ffi::{CStr, c_char, c_void};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::thread::{self, JoinHandle};

use jfn_wake_event::WakeEvent;

use crate::coordinator::Input;
use crate::types::*;

const MAX_BUFFERED_RANGES: usize = 8;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct JfnDispatcherBufferedRange {
    pub start_ticks: i64,
    pub end_ticks: i64,
}

// Mirrors MpvEventType in src/mpv/event.h.
const T_NONE: u8 = 0;
const T_SHUTDOWN: u8 = 1;
const T_FILE_LOADED: u8 = 2;
const T_END_FILE_EOF: u8 = 3;
const T_END_FILE_ERROR: u8 = 4;
const T_END_FILE_CANCEL: u8 = 5;
const T_PAUSE: u8 = 6;
const T_TIME_POS: u8 = 7;
const T_DURATION: u8 = 8;
const T_FULLSCREEN: u8 = 9;
const T_OSD_DIMS: u8 = 10;
const T_DISPLAY_SCALE: u8 = 11;
const T_SPEED: u8 = 12;
const T_SEEKING: u8 = 13;
const T_DISPLAY_FPS: u8 = 14;
const T_BUFFERED_RANGES: u8 = 15;
const T_PAUSED_FOR_CACHE: u8 = 16;
const T_CORE_IDLE: u8 = 17;
const T_VIDEO_FRAME_INFO: u8 = 18;

#[repr(C)]
pub struct JfnMpvEventC {
    pub type_: u8,
    pub flag: bool,
    pub flag2: bool, // FULLSCREEN: was_maximized
    pub dbl: f64,
    pub pw: i32,
    pub ph: i32,
    pub lw: i32,
    pub lh: i32,
    pub range_count: i32,
    pub ranges: [JfnDispatcherBufferedRange; MAX_BUFFERED_RANGES],
    pub err_msg: *const c_char, // borrowed for the duration of the publish call
}

#[derive(Clone)]
struct MpvEventOwned {
    type_: u8,
    flag: bool,
    flag2: bool,
    dbl: f64,
    pw: i32,
    ph: i32,
    lw: i32,
    lh: i32,
    ranges: Vec<PlaybackBufferedRange>,
    err_msg: Option<String>,
}

impl MpvEventOwned {
    fn from_c(c: &JfnMpvEventC) -> Self {
        let mut ranges = Vec::new();
        if c.range_count > 0 {
            let n = (c.range_count as usize).min(MAX_BUFFERED_RANGES);
            ranges.reserve(n);
            for i in 0..n {
                ranges.push(PlaybackBufferedRange {
                    start_ticks: c.ranges[i].start_ticks,
                    end_ticks: c.ranges[i].end_ticks,
                });
            }
        }
        let err_msg = if c.err_msg.is_null() {
            None
        } else {
            Some(
                unsafe { CStr::from_ptr(c.err_msg) }
                    .to_string_lossy()
                    .into_owned(),
            )
        };
        Self {
            type_: c.type_,
            flag: c.flag,
            flag2: c.flag2,
            dbl: c.dbl,
            pw: c.pw,
            ph: c.ph,
            lw: c.lw,
            lh: c.lh,
            ranges,
            err_msg,
        }
    }
}

#[derive(Clone, Copy)]
struct PumpSink {
    ctx: *mut c_void,
    fd: libc::c_int,
    pump: extern "C" fn(*mut c_void),
}
unsafe impl Send for PumpSink {}
unsafe impl Sync for PumpSink {}

struct Shared {
    queue: Mutex<VecDeque<MpvEventOwned>>,
    queue_wake: WakeEvent,
    shutdown_wake: WakeEvent,
    sinks: Mutex<Vec<PumpSink>>,
    running: AtomicBool,
    display_scale_cb: Mutex<Option<extern "C" fn(f64)>>,
}

struct State {
    shared: Arc<Shared>,
    join: Option<JoinHandle<()>>,
}

static STATE: OnceLock<Mutex<Option<State>>> = OnceLock::new();

fn state_slot() -> &'static Mutex<Option<State>> {
    STATE.get_or_init(|| Mutex::new(None))
}

fn shared() -> Option<Arc<Shared>> {
    state_slot()
        .lock()
        .unwrap()
        .as_ref()
        .map(|s| Arc::clone(&s.shared))
}

// =====================================================================
// FFI
// =====================================================================

#[unsafe(no_mangle)]
pub extern "C" fn jfn_dispatcher_init() {
    let mut guard = state_slot().lock().unwrap();
    if guard.is_some() {
        return;
    }
    *guard = Some(State {
        shared: Arc::new(Shared {
            queue: Mutex::new(VecDeque::new()),
            queue_wake: WakeEvent::new().expect("WakeEvent queue"),
            shutdown_wake: WakeEvent::new().expect("WakeEvent shutdown"),
            sinks: Mutex::new(Vec::new()),
            running: AtomicBool::new(false),
            display_scale_cb: Mutex::new(None),
        }),
        join: None,
    });
}

/// # Safety
/// `ctx` must remain valid until `jfn_dispatcher_stop`. `pump` is invoked
/// from the dispatcher thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_dispatcher_register_sink_pump(
    ctx: *mut c_void,
    fd: libc::c_int,
    pump: extern "C" fn(*mut c_void),
) {
    if let Some(s) = shared() {
        s.sinks.lock().unwrap().push(PumpSink { ctx, fd, pump });
    }
}

/// # Safety
/// `cb` must remain callable for the dispatcher's lifetime.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_dispatcher_set_display_scale_handler(cb: extern "C" fn(f64)) {
    if let Some(s) = shared() {
        *s.display_scale_cb.lock().unwrap() = Some(cb);
    }
}

/// # Safety
/// `ev` must point to a valid `JfnMpvEventC`. `err_msg`, when non-null,
/// must be a NUL-terminated C string valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_dispatcher_publish(ev: *const JfnMpvEventC) {
    if ev.is_null() {
        return;
    }
    let Some(s) = shared() else {
        return;
    };
    let owned = unsafe { MpvEventOwned::from_c(&*ev) };
    if owned.type_ == T_NONE {
        return;
    }
    s.queue.lock().unwrap().push_back(owned);
    s.queue_wake.signal();
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_dispatcher_start() {
    let mut guard = state_slot().lock().unwrap();
    let Some(state) = guard.as_mut() else {
        return;
    };
    if state.shared.running.swap(true, Ordering::SeqCst) {
        return;
    }
    let shared = Arc::clone(&state.shared);
    state.join = Some(thread::spawn(move || worker(shared)));
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_dispatcher_stop() {
    let join = {
        let mut guard = state_slot().lock().unwrap();
        let Some(state) = guard.as_mut() else {
            return;
        };
        if !state.shared.running.swap(false, Ordering::SeqCst) {
            return;
        }
        state.shared.shutdown_wake.signal();
        state.join.take()
    };
    if let Some(h) = join {
        let _ = h.join();
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_dispatcher_shutdown() {
    jfn_dispatcher_stop();
    state_slot().lock().unwrap().take();
}

// =====================================================================
// Worker
// =====================================================================

fn worker(shared: Arc<Shared>) {
    let sinks = shared.sinks.lock().unwrap().clone();
    let mut pfds: Vec<libc::pollfd> = Vec::with_capacity(sinks.len() + 2);
    pfds.push(libc::pollfd {
        fd: shared.queue_wake.fd(),
        events: libc::POLLIN,
        revents: 0,
    });
    for s in &sinks {
        pfds.push(libc::pollfd {
            fd: s.fd,
            events: libc::POLLIN,
            revents: 0,
        });
    }
    pfds.push(libc::pollfd {
        fd: shared.shutdown_wake.fd(),
        events: libc::POLLIN,
        revents: 0,
    });
    let shutdown_idx = pfds.len() - 1;

    loop {
        unsafe {
            libc::poll(pfds.as_mut_ptr(), pfds.len() as libc::nfds_t, -1);
        }
        if pfds[shutdown_idx].revents & libc::POLLIN != 0 {
            return;
        }

        // Pump sinks before draining the queue so any events the
        // coordinator emitted during the prior cycle are delivered before
        // the next mpv event flips state. (Freshness preference; not a
        // correctness requirement since sinks no longer pull from coord.)
        for (i, sink) in sinks.iter().enumerate() {
            if pfds[1 + i].revents & libc::POLLIN != 0 {
                (sink.pump)(sink.ctx);
            }
        }

        shared.queue_wake.drain();
        let work: VecDeque<MpvEventOwned> = {
            let mut q = shared.queue.lock().unwrap();
            std::mem::take(&mut *q)
        };
        for ev in work {
            route(&shared, &ev);
        }
    }
}

fn route(shared: &Shared, ev: &MpvEventOwned) {
    use crate::ffi::post;
    match ev.type_ {
        T_NONE | T_SHUTDOWN => {}
        T_FILE_LOADED => post(Input::FileLoaded),
        T_END_FILE_EOF => post(Input::EndFile {
            reason: EndReason::Eof,
            error_message: String::new(),
        }),
        T_END_FILE_ERROR => post(Input::EndFile {
            reason: EndReason::Error,
            error_message: ev.err_msg.clone().unwrap_or_default(),
        }),
        T_END_FILE_CANCEL => post(Input::EndFile {
            reason: EndReason::Canceled,
            error_message: String::new(),
        }),
        T_PAUSE => post(Input::PauseChanged(ev.flag)),
        T_TIME_POS => post(Input::Position((ev.dbl * 1_000_000.0) as i64)),
        T_DURATION => post(Input::Duration((ev.dbl * 1_000_000.0) as i64)),
        T_FULLSCREEN => post(Input::Fullscreen {
            fullscreen: ev.flag,
            was_maximized: ev.flag2,
        }),
        T_OSD_DIMS => post(Input::OsdDims {
            lw: ev.lw,
            lh: ev.lh,
            pw: ev.pw,
            ph: ev.ph,
        }),
        T_DISPLAY_SCALE => {
            if ev.dbl > 0.0 {
                if let Some(cb) = *shared.display_scale_cb.lock().unwrap() {
                    cb(ev.dbl);
                }
            }
        }
        T_SPEED => post(Input::Speed(ev.dbl)),
        T_SEEKING => post(Input::SeekingChanged(ev.flag)),
        T_DISPLAY_FPS => {
            if ev.dbl > 0.0 {
                post(Input::DisplayHz(ev.dbl));
            }
        }
        T_BUFFERED_RANGES => post(Input::BufferedRanges(ev.ranges.clone())),
        T_PAUSED_FOR_CACHE => post(Input::PausedForCache(ev.flag)),
        T_CORE_IDLE => post(Input::CoreIdle(ev.flag)),
        T_VIDEO_FRAME_INFO => post(Input::VideoFrameAvailable(ev.flag)),
        _ => {}
    }
}
