//! CefLayer state (Rust side).
//!
//! Slice 1 introduced this module with the small bits of CefLayer state that
//! have no CEF dependency: name, closed/loaded flags, condvars. Slice 3 adds
//! the resize-debounce + invalidate-loop state machine and the per-layer
//! CEF browser ops vtable that lets Rust schedule `WasResized`,
//! `NotifyScreenInfoChanged`, `Invalidate`, `SetWindowlessFrameRate`,
//! `SendExternalBeginFrame`, and `ExecuteJavaScript` calls on TID_UI.
//!
//! Lifetime model: the FFI handle is `Box<JfnCefLayer>` (raw pointer owned by
//! the C++ side). Internal state lives in an `Arc<Inner>` so posted CEF
//! tasks can keep a clone alive past `jfn_cef_layer_free`. CefLayer
//! destructor clears `cef_ops` first, so any in-flight task that does
//! eventually run sees `None` and exits.

use cef::rc::Rc;
use cef::{post_delayed_task, post_task, wrap_task, ImplTask, Task, ThreadId, WrapTask};
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::time::Instant;

use crate::platform_ops;

unsafe extern "C" {
    fn jfn_playback_display_hz() -> f64;
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct JfnCefBrowserOps {
    pub ctx: *mut c_void,
    pub notify_screen_info_changed: Option<unsafe extern "C" fn(*mut c_void)>,
    pub was_resized: Option<unsafe extern "C" fn(*mut c_void)>,
    pub invalidate: Option<unsafe extern "C" fn(*mut c_void)>,
    pub send_external_begin_frame: Option<unsafe extern "C" fn(*mut c_void)>,
    pub set_windowless_frame_rate: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
    pub exec_js: Option<unsafe extern "C" fn(*mut c_void, *const c_char, usize)>,
}

// SAFETY: the C++ side guarantees the ctx pointer remains valid until
// jfn_cef_layer_clear_browser_ops is called from the CefLayer destructor.
unsafe impl Send for JfnCefBrowserOps {}
unsafe impl Sync for JfnCefBrowserOps {}

pub struct JfnCefLayer {
    inner: Arc<Inner>,
}

const BOOST_MULTIPLIER: i32 = 2;
const INVALIDATE_TICK_LIMIT: i32 = 1000;

struct Inner {
    // identity / state queries (slice 1)
    name: Mutex<String>,
    closed: AtomicBool,
    loaded: AtomicBool,
    close_mtx: Mutex<()>,
    close_cv: Condvar,
    load_mtx: Mutex<()>,
    load_cv: Condvar,

    // CEF browser callbacks (per-layer); cleared on destruction.
    cef_ops: Mutex<Option<JfnCefBrowserOps>>,
    // Opaque per-layer surface handle (PlatformSurface*); passed back to the
    // C++ platform vtable for surface_resize / present / popup.
    surface: Mutex<*mut c_void>,

    // logical/physical dims (slice 3)
    width: AtomicI32,
    height: AtomicI32,
    physical_w: AtomicI32,
    physical_h: AtomicI32,

    // frame rate (slice 3): configured, boost-saved, last applied
    frame_rate: AtomicI32,
    saved_frame_rate: AtomicI32,
    current_frame_rate: AtomicI32,

    // resize-debounce (slice 3)
    resize_scheduled: AtomicBool,
    last_was_resized_ns: AtomicI64,
    resize_gen: AtomicU64,

    // invalidate-loop state (slice 3)
    invalidate_running: AtomicBool,
    invalidate_stop: AtomicBool,
    invalidate_tick_count: AtomicI32,

    // post-resize paint-skip / pump-stop (slice 3)
    last_paint_gen: AtomicU64,
    paints_since_resize: AtomicI32,
    skip_paints_after_resize: AtomicI32,
    pump_paint_count: AtomicI32,
    last_skip_reset_ns: AtomicI64,
}

// SAFETY: surface is a C++ pointer treated as opaque; only handed back to
// the platform vtable on TID_UI.
unsafe impl Send for Inner {}
unsafe impl Sync for Inner {}

impl Inner {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            name: Mutex::new(String::new()),
            closed: AtomicBool::new(false),
            loaded: AtomicBool::new(false),
            close_mtx: Mutex::new(()),
            close_cv: Condvar::new(),
            load_mtx: Mutex::new(()),
            load_cv: Condvar::new(),
            cef_ops: Mutex::new(None),
            surface: Mutex::new(std::ptr::null_mut()),
            width: AtomicI32::new(0),
            height: AtomicI32::new(0),
            physical_w: AtomicI32::new(0),
            physical_h: AtomicI32::new(0),
            frame_rate: AtomicI32::new(0),
            saved_frame_rate: AtomicI32::new(0),
            current_frame_rate: AtomicI32::new(0),
            resize_scheduled: AtomicBool::new(false),
            last_was_resized_ns: AtomicI64::new(0),
            resize_gen: AtomicU64::new(0),
            invalidate_running: AtomicBool::new(false),
            invalidate_stop: AtomicBool::new(false),
            invalidate_tick_count: AtomicI32::new(0),
            last_paint_gen: AtomicU64::new(0),
            paints_since_resize: AtomicI32::new(0),
            skip_paints_after_resize: AtomicI32::new(0),
            pump_paint_count: AtomicI32::new(0),
            last_skip_reset_ns: AtomicI64::new(0),
        })
    }

    fn name_str(&self) -> String {
        self.name.lock().unwrap().clone()
    }

    fn surface_ptr(&self) -> *mut c_void {
        *self.surface.lock().unwrap()
    }

    fn with_ops<R>(&self, f: impl FnOnce(&JfnCefBrowserOps) -> R) -> Option<R> {
        let g = self.cef_ops.lock().ok()?;
        g.as_ref().map(f)
    }

    fn notify_screen_info_changed(&self) {
        self.with_ops(|o| {
            if let Some(f) = o.notify_screen_info_changed {
                unsafe { f(o.ctx) }
            }
        });
    }
    fn cef_was_resized(&self) {
        self.with_ops(|o| {
            if let Some(f) = o.was_resized {
                unsafe { f(o.ctx) }
            }
        });
    }
    fn invalidate(&self) {
        self.with_ops(|o| {
            if let Some(f) = o.invalidate {
                unsafe { f(o.ctx) }
            }
        });
    }
    #[allow(dead_code)]
    fn send_external_begin_frame(&self) {
        self.with_ops(|o| {
            if let Some(f) = o.send_external_begin_frame {
                unsafe { f(o.ctx) }
            }
        });
    }
    fn cef_set_windowless_frame_rate(&self, hz: i32) {
        self.with_ops(|o| {
            if let Some(f) = o.set_windowless_frame_rate {
                unsafe { f(o.ctx, hz) }
            }
        });
    }
    fn exec_js(&self, js: &str) {
        self.with_ops(|o| {
            if let Some(f) = o.exec_js {
                unsafe { f(o.ctx, js.as_ptr() as *const c_char, js.len()) }
            }
        });
    }

    fn browser_alive(&self) -> bool {
        self.cef_ops.lock().unwrap().is_some() && !self.closed.load(Ordering::Acquire)
    }

    fn set_frame_rate(&self, hz: i32) {
        if hz <= 0 || !self.browser_alive() {
            return;
        }
        self.cef_set_windowless_frame_rate(hz);
        self.current_frame_rate.store(hz, Ordering::Release);
    }

    fn apply_pending_resize(self: &Arc<Self>) {
        self.resize_scheduled.store(false, Ordering::Release);
        if !self.browser_alive() {
            return;
        }
        let now = now_ns();
        self.last_was_resized_ns.store(now, Ordering::Release);
        // WasResized retargets the renderer; any stable-size streak (possibly
        // accumulated against the old dims while this apply was pending) must
        // be invalidated.
        self.resize_gen.fetch_add(1, Ordering::AcqRel);
        self.notify_screen_info_changed();
        self.cef_was_resized();
        self.invalidate();
        self.kick_invalidate_loop();
    }

    fn kick_invalidate_loop(self: &Arc<Self>) {
        self.invalidate_stop.store(false, Ordering::Release);
        if self
            .invalidate_running
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }
        self.invalidate_tick_count.store(0, Ordering::Release);
        let inner = Arc::clone(self);
        let mut task = KickTask::new(inner);
        let _ = post_task(ThreadId::UI, Some(&mut task));
    }

    fn kick_apply(self: &Arc<Self>) {
        // Boost CEF compositor rate while the loop is live — JS rAF ties to
        // compositor rate, so this speeds up convergence to post-resize dims.
        let fps = self.frame_rate.load(Ordering::Acquire);
        if self.browser_alive() && fps > 0 && self.saved_frame_rate.load(Ordering::Acquire) == 0 {
            self.saved_frame_rate.store(fps, Ordering::Release);
            self.set_frame_rate(fps * BOOST_MULTIPLIER);
        }
        self.invalidate_tick();
    }

    fn invalidate_tick(self: &Arc<Self>) {
        if self.invalidate_tick_count.fetch_add(1, Ordering::AcqRel) + 1 > INVALIDATE_TICK_LIMIT {
            self.invalidate_stop.store(true, Ordering::Release);
        }
        if self.invalidate_stop.load(Ordering::Acquire) {
            let saved = self.saved_frame_rate.swap(0, Ordering::AcqRel);
            if self.browser_alive() && saved > 0 {
                self.set_frame_rate(saved);
            }
            self.invalidate_running.store(false, Ordering::Release);
            return;
        }
        if self.browser_alive() {
            self.invalidate();
            #[cfg(target_os = "macos")]
            self.send_external_begin_frame();
        }
        let fps = self.frame_rate.load(Ordering::Acquire);
        if fps <= 0 {
            self.invalidate_running.store(false, Ordering::Release);
            return;
        }
        // Tick at 4x display refresh so the compositor gets nudged more
        // often than the boosted output rate (2x) — keeps frame production
        // ahead of the present cadence during a resize.
        let tick_hz = fps * 4;
        let delay_ms = ((1000.0 / tick_hz as f64) + 0.5) as i64;
        let delay_ms = delay_ms.max(1);
        let inner = Arc::clone(self);
        let mut task = TickTask::new(inner);
        let _ = post_delayed_task(ThreadId::UI, Some(&mut task), delay_ms);
    }

    fn resize(self: &Arc<Self>, w: i32, h: i32, pw: i32, ph: i32) {
        self.width.store(w, Ordering::Release);
        self.height.store(h, Ordering::Release);
        self.physical_w.store(pw, Ordering::Release);
        self.physical_h.store(ph, Ordering::Release);
        self.resize_gen.fetch_add(1, Ordering::AcqRel);

        // Wayland viewport must update on every configure to avoid stale
        // src/dst — runs immediately.
        let surface = self.surface_ptr();
        if !surface.is_null() {
            if let Some(ops) = platform_ops::ops() {
                if let Some(f) = ops.surface_resize {
                    unsafe { f(surface, w, h, pw, ph) };
                }
            }
        }

        // Defer kick until the browser exists; OnAfterCreated will fire it.
        if !self.browser_alive() {
            return;
        }

        // Debounce the CEF host notify (re-layout) to one display-refresh
        // period. Drag fires many configures per frame; coalescing them
        // saves N-1 wasted re-layouts.
        let now = now_ns();
        let hz = unsafe { jfn_playback_display_hz() };
        let period_ns = if hz > 0.0 {
            (1e9 / hz) as i64
        } else {
            16_666_667
        };
        let last = self.last_was_resized_ns.load(Ordering::Acquire);
        if now - last >= period_ns {
            self.last_was_resized_ns.store(now, Ordering::Release);
            self.notify_screen_info_changed();
            self.cef_was_resized();
            self.invalidate();
            self.kick_invalidate_loop();
            return;
        }
        // Within the debounce window — schedule a single deferred apply if
        // one isn't already pending. Latest width/height get picked up.
        if self
            .resize_scheduled
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            let delay_ms = ((period_ns - (now - last)) / 1_000_000).max(1);
            let inner = Arc::clone(self);
            let mut task = ApplyResizeTask::new(inner);
            let _ = post_delayed_task(ThreadId::UI, Some(&mut task), delay_ms);
        }
        self.kick_invalidate_loop();
    }

    fn set_refresh_rate(self: &Arc<Self>, hz: f64) {
        if hz <= 0.0 {
            return;
        }
        let target = hz.ceil() as i32;
        let inner = Arc::clone(self);
        let mut task = SetRefreshTask::new(inner, target);
        let _ = post_task(ThreadId::UI, Some(&mut task));
    }

    fn apply_set_refresh(&self, target: i32) {
        self.frame_rate.store(target, Ordering::Release);
        // If a nudge-loop boost is active, just update what we'll restore to
        // and let the boost rate keep running. Otherwise apply now.
        if self.saved_frame_rate.load(Ordering::Acquire) > 0 {
            self.saved_frame_rate.store(target, Ordering::Release);
        } else {
            self.set_frame_rate(target);
        }
    }

    fn should_present_paint(&self) -> bool {
        let cur_gen = self.resize_gen.load(Ordering::Acquire);
        let last_gen = self.last_paint_gen.load(Ordering::Acquire);
        if cur_gen != last_gen {
            self.last_paint_gen.store(cur_gen, Ordering::Release);
            // Rate-clamp the skip-counter reset. Continuous drag bumps gen
            // many times per second; resetting on every bump would keep
            // wiping the counter before any paint clears the skip threshold.
            let now_ns_val = now_ns();
            let hz = unsafe { jfn_playback_display_hz() };
            let period_ns = if hz > 0.0 {
                (1e9 / hz) as i64
            } else {
                16_666_667
            };
            if now_ns_val - self.last_skip_reset_ns.load(Ordering::Acquire) >= period_ns {
                self.last_skip_reset_ns.store(now_ns_val, Ordering::Release);
                let fps = self.frame_rate.load(Ordering::Acquire);
                self.skip_paints_after_resize.store(1, Ordering::Release);
                self.pump_paint_count
                    .store(if fps > 0 { 1 + fps } else { 0 }, Ordering::Release);
                self.paints_since_resize.store(0, Ordering::Release);
            }
        }
        let count = self.paints_since_resize.fetch_add(1, Ordering::AcqRel) + 1;
        let skip = self.skip_paints_after_resize.load(Ordering::Acquire);
        let pump = self.pump_paint_count.load(Ordering::Acquire);
        let present = count > skip;
        if pump > 0 && count == pump {
            // Pumped enough frames — signal stop to host Invalidate loop and
            // renderer's rAF loop. Counter remains past pump so subsequent
            // paints don't re-fire.
            self.invalidate_stop.store(true, Ordering::Release);
            self.exec_js("window.__cefStopRaf && window.__cefStopRaf();");
        }
        present
    }
}

fn now_ns() -> i64 {
    static ORIGIN: OnceLock<Instant> = OnceLock::new();
    Instant::now()
        .duration_since(*ORIGIN.get_or_init(Instant::now))
        .as_nanos() as i64
}

// ---------------------------------------------------------------------------
// CEF Task wrappers (post_task / post_delayed_task targets)
// ---------------------------------------------------------------------------

wrap_task! {
    struct ApplyResizeTask {
        inner: Arc<Inner>,
    }
    impl Task {
        fn execute(&self) {
            self.inner.apply_pending_resize();
        }
    }
}

wrap_task! {
    struct KickTask {
        inner: Arc<Inner>,
    }
    impl Task {
        fn execute(&self) {
            self.inner.kick_apply();
        }
    }
}

wrap_task! {
    struct TickTask {
        inner: Arc<Inner>,
    }
    impl Task {
        fn execute(&self) {
            self.inner.invalidate_tick();
        }
    }
}

wrap_task! {
    struct SetRefreshTask {
        inner: Arc<Inner>,
        target: i32,
    }
    impl Task {
        fn execute(&self) {
            self.inner.apply_set_refresh(self.target);
        }
    }
}

// ---------------------------------------------------------------------------
// FFI surface
// ---------------------------------------------------------------------------

unsafe fn arc(h: *const JfnCefLayer) -> Arc<Inner> {
    Arc::clone(unsafe { &(*h).inner })
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_cef_layer_new() -> *mut JfnCefLayer {
    Box::into_raw(Box::new(JfnCefLayer {
        inner: Inner::new(),
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_free(h: *mut JfnCefLayer) {
    if h.is_null() {
        return;
    }
    drop(unsafe { Box::from_raw(h) });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_set_name(h: *const JfnCefLayer, s: *const c_char) {
    let inner = unsafe { arc(h) };
    let new = if s.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(s) }.to_string_lossy().into_owned()
    };
    *inner.name.lock().unwrap() = new;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_is_closed(h: *const JfnCefLayer) -> bool {
    unsafe { arc(h) }.closed.load(Ordering::Acquire)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_is_loaded(h: *const JfnCefLayer) -> bool {
    unsafe { arc(h) }.loaded.load(Ordering::Acquire)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_set_closed(h: *const JfnCefLayer, v: bool) {
    let l = unsafe { arc(h) };
    let _g = l.close_mtx.lock().unwrap();
    l.closed.store(v, Ordering::Release);
    l.close_cv.notify_all();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_set_loaded(h: *const JfnCefLayer, v: bool) {
    let l = unsafe { arc(h) };
    let _g = l.load_mtx.lock().unwrap();
    l.loaded.store(v, Ordering::Release);
    l.load_cv.notify_all();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_wait_for_close(h: *const JfnCefLayer) {
    let l = unsafe { arc(h) };
    let mut g = l.close_mtx.lock().unwrap();
    while !l.closed.load(Ordering::Acquire) {
        g = l.close_cv.wait(g).unwrap();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_wait_for_load(h: *const JfnCefLayer) {
    let l = unsafe { arc(h) };
    let mut g = l.load_mtx.lock().unwrap();
    while !l.loaded.load(Ordering::Acquire) {
        g = l.load_cv.wait(g).unwrap();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_set_browser_ops(
    h: *const JfnCefLayer,
    ops: *const JfnCefBrowserOps,
) {
    let inner = unsafe { arc(h) };
    *inner.cef_ops.lock().unwrap() = if ops.is_null() {
        None
    } else {
        Some(unsafe { *ops })
    };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_clear_browser_ops(h: *const JfnCefLayer) {
    *unsafe { arc(h) }.cef_ops.lock().unwrap() = None;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_set_surface(h: *const JfnCefLayer, s: *mut c_void) {
    *unsafe { arc(h) }.surface.lock().unwrap() = s;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_resize(
    h: *const JfnCefLayer,
    w: c_int,
    height: c_int,
    pw: c_int,
    ph: c_int,
) {
    unsafe { arc(h) }.resize(w, height, pw, ph);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_set_refresh_rate(h: *const JfnCefLayer, hz: f64) {
    unsafe { arc(h) }.set_refresh_rate(hz);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_kick_invalidate_loop(h: *const JfnCefLayer) {
    unsafe { arc(h) }.kick_invalidate_loop();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_should_present_paint(h: *const JfnCefLayer) -> bool {
    unsafe { arc(h) }.should_present_paint()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_get_view_rect(
    h: *const JfnCefLayer,
    out_w: *mut c_int,
    out_h: *mut c_int,
) {
    let l = unsafe { arc(h) };
    unsafe {
        *out_w = l.width.load(Ordering::Acquire);
        *out_h = l.height.load(Ordering::Acquire);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_get_screen_info(
    h: *const JfnCefLayer,
    out_scale: *mut f32,
    out_w: *mut c_int,
    out_h: *mut c_int,
) {
    let l = unsafe { arc(h) };
    let w = l.width.load(Ordering::Acquire);
    let pw = l.physical_w.load(Ordering::Acquire);
    let scale = if pw > 0 && w > 0 {
        pw as f32 / w as f32
    } else {
        1.0
    };
    unsafe {
        *out_scale = scale;
        *out_w = w;
        *out_h = l.height.load(Ordering::Acquire);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_frame_rate(h: *const JfnCefLayer) -> c_int {
    unsafe { arc(h) }.frame_rate.load(Ordering::Acquire)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_bump_resize_gen(h: *const JfnCefLayer) {
    unsafe { arc(h) }.resize_gen.fetch_add(1, Ordering::AcqRel);
}

// Marks the invalidate loop for stop on the next tick. Called from
// OnBeforeClose on the C++ side; ensures the posted-task Arc clones drop and
// the layer can finish destruction.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_stop_invalidate(h: *const JfnCefLayer) {
    unsafe { arc(h) }
        .invalidate_stop
        .store(true, Ordering::Release);
}

// Read the layer name back as a heap-allocated C string. Caller must free
// with jfn_cef_layer_free_string. Used by C++ for log lines after slice 9.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_name_dup(h: *const JfnCefLayer) -> *mut c_char {
    let s = unsafe { arc(h) }.name_str();
    match std::ffi::CString::new(s) {
        Ok(c) => c.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_layer_free_string(p: *mut c_char) {
    if p.is_null() {
        return;
    }
    drop(unsafe { std::ffi::CString::from_raw(p) });
}
