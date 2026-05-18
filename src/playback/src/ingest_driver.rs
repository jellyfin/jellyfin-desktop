//! Adapters wiring [`crate::ingest`] to the rest of the world:
//! the global [`IngestState`], C ABI entry points for the C++ mpv event
//! thread, and the side-channel callbacks (display scale, window
//! pixels, shutdown) that don't flow through the coordinator queue.
//!
//! Replaces the legacy `src/playback/jfn_dispatcher.h` API and the C++
//! `digest_property` switch in `src/mpv/event.cpp`.

use std::ffi::c_void;
use std::os::raw::c_int;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::{self, JoinHandle};

use jfn_mpv::{Event, ObserveId, PropertyValue, sys as mpv_sys};

use crate::ffi::post as post_input;
use crate::ingest::{
    IngestCtx, IngestOut, IngestState, ingest_event_for_ffi, ingest_property_for_ffi,
};

// ---------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------

fn state() -> &'static IngestState {
    static STATE: OnceLock<IngestState> = OnceLock::new();
    STATE.get_or_init(IngestState::new)
}

/// Returned by [`jfn_playback_ingest_mpv_event`] as a bitfield:
///   bit 0 — `MPV_EVENT_SHUTDOWN` reached; caller should break its loop.
pub const INGEST_FLAG_SHUTDOWN: u8 = 1;

/// `(scale, has_macos_logical, mac_lw, mac_lh)` snapshot supplied per
/// event by the C++ caller.
struct CallerCtx {
    scale: f32,
    mac: Option<(i32, i32)>,
}

impl IngestCtx for CallerCtx {
    fn scale(&self) -> f32 {
        self.scale
    }
    fn macos_logical_size(&self) -> Option<(i32, i32)> {
        self.mac
    }
}

// ---------------------------------------------------------------------
// Side-channel callbacks (display scale, window pixels)
// ---------------------------------------------------------------------

type DisplayScaleCb = extern "C" fn(f64);

fn display_scale_slot() -> &'static std::sync::Mutex<Option<DisplayScaleCb>> {
    static SLOT: OnceLock<std::sync::Mutex<Option<DisplayScaleCb>>> = OnceLock::new();
    SLOT.get_or_init(|| std::sync::Mutex::new(None))
}

fn shutdown_flag() -> &'static AtomicBool {
    static FLAG: AtomicBool = AtomicBool::new(false);
    &FLAG
}

// ---------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------

fn dispatch(outs: Vec<IngestOut>) -> u8 {
    let mut flags = 0u8;
    for o in outs {
        match o {
            IngestOut::Input(i) => post_input(i),
            IngestOut::DisplayScaleChanged(d) => {
                if let Some(cb) = *display_scale_slot().lock().unwrap() {
                    cb(d);
                }
            }
            IngestOut::Shutdown => {
                shutdown_flag().store(true, Ordering::Release);
                flags |= INGEST_FLAG_SHUTDOWN;
            }
        }
    }
    flags
}

// ---------------------------------------------------------------------
// FFI
// ---------------------------------------------------------------------

/// Install the browser-side `setScale` thunk used to resolve
/// `DISPLAY_SCALE` property changes. Replaces any prior callback.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_display_scale_handler(cb: DisplayScaleCb) {
    *display_scale_slot().lock().unwrap() = Some(cb);
}

/// Push a device-pixel window size into the geometry-save cache.
/// Mirrors the legacy `mpv::set_window_pixels` producer used at boot
/// (geometry seed) and runtime resize.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_window_pixels(pw: c_int, ph: c_int) {
    state().set_window_pixels(pw as i32, ph as i32);
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_window_pw() -> c_int {
    state().window_pw() as c_int
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_window_ph() -> c_int {
    state().window_ph() as c_int
}

/// Decode one raw `mpv_event*` (returned by `mpv_wait_event`) into
/// coordinator inputs + side-channel callbacks. Returns flag bits — see
/// [`INGEST_FLAG_SHUTDOWN`].
///
/// `has_macos_logical` set to non-zero signals that `mac_lw`/`mac_lh`
/// carry a valid macOS logical-content size override. Non-macOS callers
/// pass `false` / zeros.
///
/// # Safety
/// `ev` must be a pointer returned by `mpv_wait_event` and not yet
/// invalidated by a subsequent call on the same handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_playback_ingest_mpv_event(
    ev: *const c_void,
    scale: f32,
    has_macos_logical: bool,
    mac_lw: c_int,
    mac_lh: c_int,
) -> u8 {
    if ev.is_null() {
        return 0;
    }
    let event = unsafe { Event::from_raw(ev as *const mpv_sys::mpv_event) };
    let ctx = CallerCtx {
        scale,
        mac: if has_macos_logical {
            Some((mac_lw as i32, mac_lh as i32))
        } else {
            None
        },
    };
    let outs = ingest_event_for_ffi(&event, state(), &ctx);
    dispatch(outs)
}

/// Push synthetic OSD-dim pixels through the same digest path the
/// `osd-dimensions` property observation drives. Used by the Wayland
/// xdg_toplevel.configure intercept (`platform::wayland::on_proxy_configure`)
/// in place of mpv's own osd-dimensions delivery.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_post_osd_pixels(
    pw: c_int,
    ph: c_int,
    scale: f32,
    has_macos_logical: bool,
    mac_lw: c_int,
    mac_lh: c_int,
) {
    use jfn_mpv::Node;
    let node = Node::Map(vec![
        ("w".into(), Node::Int(pw as i64)),
        ("h".into(), Node::Int(ph as i64)),
    ]);
    let ctx = CallerCtx {
        scale,
        mac: if has_macos_logical {
            Some((mac_lw as i32, mac_lh as i32))
        } else {
            None
        },
    };
    let outs = ingest_property_for_ffi(
        OSD_DIMS_OBSERVE_ID,
        &PropertyValue::Node(node),
        state(),
        &ctx,
    );
    dispatch(outs);
}

const OSD_DIMS_OBSERVE_ID: ObserveId = crate::ingest::observe_id::OSD_DIMS;

// ---------------------------------------------------------------------
// State accessors mirroring the legacy `mpv::*` getters
// ---------------------------------------------------------------------

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_fullscreen() -> bool {
    state().fullscreen()
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_window_maximized() -> bool {
    state().window_maximized()
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_osd_pw() -> c_int {
    state().osd_pw() as c_int
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_osd_ph() -> c_int {
    state().osd_ph() as c_int
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_display_scale() -> f64 {
    state().display_scale()
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_display_hz() -> f64 {
    state().display_hz()
}

/// Seed the display-hz cache from a synchronous probe (call only from a
/// non-event context — sync mpv property reads from inside the event
/// thread deadlock).
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_display_hz(hz: f64) {
    state().set_display_hz(hz);
}

// ---------------------------------------------------------------------
// Rust-owned mpv event thread
// ---------------------------------------------------------------------

type ScaleProvider = extern "C" fn() -> f32;
type MacosLogicalProvider = extern "C" fn(*mut c_int, *mut c_int) -> bool;
type FullscreenHandler = extern "C" fn(bool);
type ShutdownHandler = extern "C" fn();

fn scale_slot() -> &'static std::sync::Mutex<Option<ScaleProvider>> {
    static SLOT: OnceLock<std::sync::Mutex<Option<ScaleProvider>>> = OnceLock::new();
    SLOT.get_or_init(|| std::sync::Mutex::new(None))
}

fn macos_logical_slot() -> &'static std::sync::Mutex<Option<MacosLogicalProvider>> {
    static SLOT: OnceLock<std::sync::Mutex<Option<MacosLogicalProvider>>> = OnceLock::new();
    SLOT.get_or_init(|| std::sync::Mutex::new(None))
}

fn fullscreen_handler_slot() -> &'static std::sync::Mutex<Option<FullscreenHandler>> {
    static SLOT: OnceLock<std::sync::Mutex<Option<FullscreenHandler>>> = OnceLock::new();
    SLOT.get_or_init(|| std::sync::Mutex::new(None))
}

fn shutdown_handler_slot() -> &'static std::sync::Mutex<Option<ShutdownHandler>> {
    static SLOT: OnceLock<std::sync::Mutex<Option<ShutdownHandler>>> = OnceLock::new();
    SLOT.get_or_init(|| std::sync::Mutex::new(None))
}

struct EventThread {
    stop: std::sync::Arc<AtomicBool>,
    join: Option<JoinHandle<()>>,
}

fn event_thread_slot() -> &'static std::sync::Mutex<Option<EventThread>> {
    static SLOT: OnceLock<std::sync::Mutex<Option<EventThread>>> = OnceLock::new();
    SLOT.get_or_init(|| std::sync::Mutex::new(None))
}

/// Install the platform fullscreen-state thunk. Invoked from the Rust
/// event thread when the `fullscreen` property changes — the C++
/// platform vtable is not bridged into Rust, so the call must run
/// through this handler.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_fullscreen_handler(cb: FullscreenHandler) {
    *fullscreen_handler_slot().lock().unwrap() = Some(cb);
}

/// Install the per-event scale provider used when normalizing OSD
/// dimensions. Must return the device pixel scale (> 0); zero or
/// negative is substituted with 1.0.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_scale_provider(cb: ScaleProvider) {
    *scale_slot().lock().unwrap() = Some(cb);
}

/// Install the macOS logical-content-size override provider. The
/// callback fills `*lw` / `*lh` and returns `true` when an override
/// applies. Non-macOS callers should leave this unset.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_macos_logical_provider(cb: MacosLogicalProvider) {
    *macos_logical_slot().lock().unwrap() = Some(cb);
}

/// Install the `MPV_EVENT_SHUTDOWN` handler. The C++ side wires this
/// to `initiate_shutdown()`.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_set_shutdown_handler(cb: ShutdownHandler) {
    *shutdown_handler_slot().lock().unwrap() = Some(cb);
}

fn snapshot_scale() -> f32 {
    let cb = *scale_slot().lock().unwrap();
    let s = cb.map(|f| f()).unwrap_or(1.0);
    if s > 0.0 { s } else { 1.0 }
}

fn snapshot_macos_logical() -> Option<(i32, i32)> {
    let cb = (*macos_logical_slot().lock().unwrap())?;
    let mut lw: c_int = 0;
    let mut lh: c_int = 0;
    if cb(&mut lw, &mut lh) {
        Some((lw as i32, lh as i32))
    } else {
        None
    }
}

fn invoke_fullscreen_handler(f: bool) {
    if let Some(cb) = *fullscreen_handler_slot().lock().unwrap() {
        cb(f);
    }
}

fn invoke_shutdown_handler() {
    if let Some(cb) = *shutdown_handler_slot().lock().unwrap() {
        cb();
    }
}

/// Spawn the Rust-owned mpv event thread. The thread blocks in
/// `mpv_wait_event(-1)` on the handle returned by
/// `jfn_mpv::boot::current_raw_handle()`, decodes each event into
/// `jfn_mpv::Event`, and routes through the same ingest path that
/// [`jfn_playback_ingest_mpv_event`] uses. Returns `false` if the
/// handle is not yet initialized or the thread is already running.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_start_mpv_event_thread() -> bool {
    let mut guard = event_thread_slot().lock().unwrap();
    if guard.is_some() {
        return false;
    }
    let Some(raw) = jfn_mpv::boot::current_raw_handle() else {
        return false;
    };
    let raw_addr = raw as usize;
    let stop = std::sync::Arc::new(AtomicBool::new(false));
    let stop_thread = std::sync::Arc::clone(&stop);
    let join = thread::Builder::new()
        .name("jfn-mpv-events".into())
        .spawn(move || event_loop(raw_addr, stop_thread))
        .expect("spawn jfn-mpv-events thread");
    *guard = Some(EventThread {
        stop,
        join: Some(join),
    });
    true
}

/// Stop the Rust-owned mpv event thread and join it. Idempotent.
/// `mpv_wakeup` is called on the live handle so the in-flight
/// `mpv_wait_event` returns immediately.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_playback_stop_mpv_event_thread() {
    let entry = event_thread_slot().lock().unwrap().take();
    let Some(mut t) = entry else { return };
    t.stop.store(true, Ordering::Release);
    jfn_mpv::boot::wakeup_current();
    if let Some(join) = t.join.take() {
        let _ = join.join();
    }
}

fn event_loop(handle_addr: usize, stop: std::sync::Arc<AtomicBool>) {
    let handle = handle_addr as *mut mpv_sys::mpv_handle;
    loop {
        if stop.load(Ordering::Acquire) {
            return;
        }
        let ev_ptr = unsafe { mpv_sys::mpv_wait_event(handle, -1.0) };
        let event = unsafe { Event::from_raw(ev_ptr) };
        match event {
            Event::None => continue,
            Event::LogMessage(ref m) => {
                jfn_mpv::forward_log_to_tracing(m);
                continue;
            }
            Event::PropertyChange { id, ref value, .. } => {
                if id == crate::ingest::observe_id::FULLSCREEN {
                    if let PropertyValue::Flag(f) = value {
                        invoke_fullscreen_handler(*f);
                    }
                }
            }
            _ => {}
        }
        let scale = snapshot_scale();
        let mac = snapshot_macos_logical();
        let ctx = CallerCtx { scale, mac };
        let outs = ingest_event_for_ffi(&event, state(), &ctx);
        let flags = dispatch(outs);
        if flags & INGEST_FLAG_SHUTDOWN != 0 {
            invoke_shutdown_handler();
            return;
        }
    }
}
