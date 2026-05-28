//! CefLayer state.
//!
//! Holds the small bits of CefLayer state plus the resize-debounce +
//! invalidate-loop state machine and the per-layer CEF browser ops dispatch
//! that schedules `WasResized`, `NotifyScreenInfoChanged`, `Invalidate`,
//! `SetWindowlessFrameRate`, `SendExternalBeginFrame`, and
//! `ExecuteJavaScript` calls on TID_UI.
//!
//! Lifetime model: the FFI handle is `Box<JfnCefLayer>` (raw pointer owned
//! by the caller). Internal state lives in an `Arc<Inner>` so posted CEF
//! tasks can keep a clone alive past `jfn_cef_layer_free`. CefLayer
//! destructor clears `cef_ops` first, so any in-flight task that does
//! eventually run sees `None` and exits.

use cef::rc::Rc;
use cef::{
    Browser, BrowserHost, BrowserSettings, CefString, Frame, ImplBrowser, ImplBrowserHost,
    ImplFrame, ImplListValue, ImplProcessMessage, ImplRunContextMenuCallback, ImplTask, MenuId,
    MouseEvent, PaintElementType, ProcessId, RunContextMenuCallback, Task, ThreadId, WindowInfo,
    WrapTask, browser_host_create_browser, post_delayed_task, post_task, process_message_create,
    sys, wrap_task,
};
use parking_lot::{Condvar, Mutex};
use std::os::raw::{c_int, c_void};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicPtr, AtomicU64, Ordering};
use std::sync::mpsc::SyncSender;
use std::sync::{Arc, OnceLock};
use std::time::Instant;

use crate::platform_ops;

use jfn_playback::ingest_driver::jfn_playback_display_hz;
use jfn_playback::shutdown::jfn_shutting_down;

mod ffi;
pub(crate) use ffi::*;
pub use ffi::{jfn_cef_layer_create, jfn_cef_layer_wait_for_load};

const STATE_NORMAL: i32 = 0;
const STATE_PENDING_RESET: i32 = 1;
const STATE_RECREATING: i32 = 2;

#[repr(C)]
pub struct JfnCefLayer {
    pub(crate) inner: Arc<Inner>,
}

// Process-wide defaults set once at startup by Browsers ctor; consumed by
// Inner::do_create_browser when building WindowInfo + BrowserSettings.
static DEFAULT_FRAME_RATE: AtomicI32 = AtomicI32::new(60);
static USE_SHARED_TEXTURES: AtomicBool = AtomicBool::new(false);

const BOOST_MULTIPLIER: i32 = 2;
const INVALIDATE_TICK_LIMIT: i32 = 1000;

pub(crate) struct Inner {
    // identity / state queries (slice 1)
    name: Mutex<String>,
    closed: AtomicBool,
    loaded: AtomicBool,
    close_mtx: Mutex<()>,
    close_cv: Condvar,
    load_mtx: Mutex<()>,
    load_cv: Condvar,

    // Stored cef::Browser captured at LifeSpanHandler::on_after_created.
    // All CEF host/frame ops on TID_UI route through this; dropped on
    // OnBeforeClose.
    browser: Mutex<Option<Browser>>,
    // Pending RunContextMenuCallback — held while the JS-rendered menu is
    // open. Cleared on menuItemSelected / menuDismissed IPC.
    pending_menu_callback: Mutex<Option<RunContextMenuCallback>>,
    // Injection-profile kind ("web" / "overlay" / "about") — looked up at
    // browser-create time to build the extra_info DictionaryValue.
    injection_kind: Mutex<String>,
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

    // popup state (slice 4). Owned 1:1 with the platform surface; each
    // CefLayer owns its popup on the platform side. Two-phase reveal: rect
    // arrives via OnPopupSize, options via the "popupOptions" renderer IPC;
    // try_show_popup fires when popup_visible + size_received + options_received.
    popup: Mutex<PopupState>,

    // lifecycle / reset state machine (slice 5)
    state: AtomicI32,
    pending_url: Mutex<String>,
    has_browser: AtomicBool,
    pending_internal_reset: AtomicBool,

    // app-level callback slots, stored as boxed closures.
    message_handler: Mutex<Option<Box<MessageFn>>>,
    created_callback: Mutex<Option<Box<CreatedFn>>>,
    before_close_callback: Mutex<Option<Box<BeforeCloseFn>>>,
    context_menu_builder: Mutex<Option<Box<ContextBuilderFn>>>,
    context_menu_dispatcher: Mutex<Option<Box<ContextDispatcherFn>>>,

    // Back-pointer to the owning `Box<JfnCefLayer>` raw handle. Set once
    // after `Box::into_raw` in `jfn_cef_layer_new`; in
    // `handle_on_before_close` we `swap` to null and act on the prior value.
    // `AtomicPtr` (not `OnceLock`) because the null sentinel after swap is
    // load-bearing — it guarantees the auto-remove + log fires exactly
    // once even if `OnBeforeClose` were ever re-entered, and prevents a
    // double-free of the registry entry.
    layer_ptr: AtomicPtr<JfnCefLayer>,
}

// Typed closure signatures stored in each callback slot. `*mut c_void` args
// stay raw because callers may want to receive cef-rs handles or C++
// CefRefPtr objects depending on which side installed the handler.
pub type MessageFn = dyn Fn(&str, *mut c_void, *mut c_void) -> bool + Send + Sync;
pub type CreatedFn = dyn Fn(*mut c_void) + Send + Sync;
pub type BeforeCloseFn = dyn Fn() + Send + Sync;
pub type ContextBuilderFn = dyn Fn(*mut c_void) + Send + Sync;
pub type ContextDispatcherFn = dyn Fn(c_int) -> bool + Send + Sync;

#[derive(Default)]
struct PopupState {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    visible: bool,
    options: Vec<String>,
    selected_idx: i32,
    size_received: bool,
    options_received: bool,
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
            browser: Mutex::new(None),
            pending_menu_callback: Mutex::new(None),
            injection_kind: Mutex::new(String::new()),
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
            popup: Mutex::new(PopupState {
                selected_idx: -1,
                ..PopupState::default()
            }),
            state: AtomicI32::new(STATE_NORMAL),
            pending_url: Mutex::new(String::new()),
            has_browser: AtomicBool::new(false),
            pending_internal_reset: AtomicBool::new(false),
            message_handler: Mutex::new(None),
            created_callback: Mutex::new(None),
            before_close_callback: Mutex::new(None),
            context_menu_builder: Mutex::new(None),
            context_menu_dispatcher: Mutex::new(None),
            layer_ptr: AtomicPtr::new(std::ptr::null_mut()),
        })
    }

    fn name_str(&self) -> String {
        self.name.lock().clone()
    }

    pub(crate) fn set_layer_ptr(&self, p: *mut JfnCefLayer) {
        self.layer_ptr.store(p, Ordering::Release);
    }

    /// Current raw layer ptr, or null after `handle_on_before_close` swap.
    /// Callbacks fired by `Inner` (created/before-close/etc.) read this to
    /// route to ptr-keyed APIs (e.g. `jfn_browsers_set_active`) without
    /// capturing the raw ptr into the closure.
    pub(crate) fn layer_ptr(&self) -> *mut JfnCefLayer {
        self.layer_ptr.load(Ordering::Acquire)
    }

    fn surface_ptr(&self) -> *mut c_void {
        *self.surface.lock()
    }

    fn browser_clone(&self) -> Option<Browser> {
        self.browser.lock().clone()
    }

    fn host(&self) -> Option<BrowserHost> {
        self.browser_clone().and_then(|b| b.host())
    }

    /// Force-close this layer's CefBrowser. No-op when no browser is alive.
    /// Operates only on `Arc<Inner>` so callers don't need to keep a raw
    /// `*mut JfnCefLayer` ptr live across this call.
    pub(crate) fn close_browser_force(&self) {
        if let Some(host) = self.host() {
            host.close_browser(1);
        }
    }

    fn focused_or_main(&self) -> Option<Frame> {
        let b = self.browser_clone()?;
        b.focused_frame().or_else(|| b.main_frame())
    }

    fn notify_screen_info_changed(&self) {
        if let Some(h) = self.host() {
            h.notify_screen_info_changed();
        }
    }
    fn cef_was_resized(&self) {
        if let Some(h) = self.host() {
            h.was_resized();
        }
    }
    fn invalidate(&self) {
        if let Some(h) = self.host() {
            h.invalidate(PaintElementType::VIEW);
        }
    }
    #[cfg(target_os = "macos")]
    fn send_external_begin_frame(&self) {
        if let Some(h) = self.host() {
            h.send_external_begin_frame();
        }
    }
    fn cef_set_windowless_frame_rate(&self, hz: i32) {
        if let Some(h) = self.host() {
            h.set_windowless_frame_rate(hz);
        }
    }
    pub(crate) fn exec_js(&self, js: &str) {
        let Some(b) = self.browser_clone() else {
            return;
        };
        let Some(f) = b.main_frame() else { return };
        let code = CefString::from(js);
        f.execute_java_script(Some(&code), Some(&CefString::from("")), 0);
    }
    fn send_process_message_named(&self, name: &str) {
        let Some(f) = self.focused_or_main() else {
            return;
        };
        let Some(mut msg) = process_message_create(Some(&CefString::from(name))) else {
            return;
        };
        f.send_process_message(
            ProcessId::from(sys::cef_process_id_t::PID_RENDERER),
            Some(&mut msg),
        );
    }
    fn cef_create_browser(self: &Arc<Self>, url: &str) {
        // WindowInfo: windowless OSR. shared_texture_enabled comes from the
        // process-wide flag set by Browsers ctor; external_begin_frame is on
        // macOS only (CVDisplayLink drives BeginFrames there).
        let shared = USE_SHARED_TEXTURES.load(Ordering::Acquire);
        let parent: sys::cef_window_handle_t = unsafe { std::mem::zeroed() };
        let mut wi = WindowInfo::default().set_as_windowless(parent);
        wi.shared_texture_enabled = if shared { 1 } else { 0 };
        #[cfg(target_os = "macos")]
        {
            wi.external_begin_frame_enabled = 1;
        }
        #[cfg(not(target_os = "macos"))]
        {
            wi.external_begin_frame_enabled = 0;
        }

        let fr_layer = self.frame_rate.load(Ordering::Acquire);
        let fr_default = DEFAULT_FRAME_RATE.load(Ordering::Acquire);
        let fr = if fr_layer > 0 { fr_layer } else { fr_default };
        let bs = BrowserSettings {
            background_color: 0,
            windowless_frame_rate: if fr > 0 { fr } else { 60 },
            ..BrowserSettings::default()
        };

        let kind = self.injection_kind.lock().clone();
        let add_ctx_menu = self.context_menu_builder.lock().is_some();
        let extra = crate::injection::build_for_kind(&kind, add_ctx_menu);

        let mut client = crate::client_impl::make_client(Arc::clone(self));
        let url_cef = CefString::from(url);
        let mut extra_opt = extra;
        let _ = browser_host_create_browser(
            Some(&wi),
            Some(&mut client),
            Some(&url_cef),
            Some(&bs),
            extra_opt.as_mut(),
            None,
        );
    }
    fn cef_close_browser(&self) {
        if let Some(h) = self.host() {
            h.close_browser(1);
        }
    }
    fn cef_load_url(&self, url: &str) {
        let Some(b) = self.browser_clone() else {
            return;
        };
        let Some(f) = b.main_frame() else { return };
        f.load_url(Some(&CefString::from(url)));
    }
    fn exec_js_focused(&self, js: &str) {
        let Some(f) = self.focused_or_main() else {
            return;
        };
        let code = CefString::from(js);
        let url_uf = f.url();
        let url = CefString::from(&url_uf);
        f.execute_java_script(Some(&code), Some(&url), 0);
    }
    fn dispatch_popup_selection(&self, idx: i32) {
        if self.closed.load(Ordering::Acquire) {
            return;
        }
        if let Some(f) = self.focused_or_main()
            && let Some(mut msg) =
                process_message_create(Some(&CefString::from("applyPopupSelection")))
        {
            if let Some(args) = msg.argument_list() {
                args.set_int(0, idx);
            }
            f.send_process_message(
                ProcessId::from(sys::cef_process_id_t::PID_RENDERER),
                Some(&mut msg),
            );
        }
        // Only public path to CancelWidget on a CEF OSR popup is a mouse-wheel
        // event outside popup_position_ — render_widget_host_view_osr.cc:1337-1343.
        if let Some(h) = self.host() {
            let me = MouseEvent {
                x: -1,
                y: -1,
                modifiers: 0,
            };
            h.send_mouse_wheel_event(Some(&me), 0, 1);
        }
    }
    fn frame_paste(&self) {
        if let Some(f) = self.focused_or_main() {
            f.paste();
        }
    }
    fn frame_undo(&self) {
        if let Some(f) = self.focused_or_main() {
            f.undo();
        }
    }
    fn frame_redo(&self) {
        if let Some(f) = self.focused_or_main() {
            f.redo();
        }
    }
    fn frame_cut(&self) {
        if let Some(f) = self.focused_or_main() {
            f.cut();
        }
    }
    fn frame_copy(&self) {
        if let Some(f) = self.focused_or_main() {
            f.copy();
        }
    }
    fn frame_select_all(&self) {
        if let Some(f) = self.focused_or_main() {
            f.select_all();
        }
    }

    fn browser_alive(&self) -> bool {
        self.browser.lock().is_some() && !self.closed.load(Ordering::Acquire)
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
        if !surface.is_null()
            && let Some(p) = platform_ops::ops()
        {
            p.surface_resize(
                surface,
                platform_ops::SurfaceSize {
                    logical_w: w,
                    logical_h: h,
                    physical_w: pw,
                    physical_h: ph,
                },
            );
        }

        // Defer kick until the browser exists; OnAfterCreated will fire it.
        if !self.browser_alive() {
            return;
        }

        // Debounce the CEF host notify (re-layout) to one display-refresh
        // period. Drag fires many configures per frame; coalescing them
        // saves N-1 wasted re-layouts.
        let now = now_ns();
        let hz = jfn_playback_display_hz();
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

    // ---- popup -----------------------------------------------------------

    fn reset_popup_state(p: &mut PopupState) {
        p.size_received = false;
        p.options_received = false;
        p.options.clear();
        p.selected_idx = -1;
    }

    pub(crate) fn on_popup_show(&self, show: bool) {
        {
            let mut p = self.popup.lock();
            p.visible = show;
            Self::reset_popup_state(&mut p);
        }
        if !show {
            let surface = self.surface_ptr();
            if !surface.is_null()
                && let Some(p) = platform_ops::ops()
            {
                p.popup_hide(surface);
            }
            return;
        }
        // Ask the renderer to walk the focused <select> and ship the option
        // list back via the "popupOptions" IPC. Reply lands in OnProcessMessage
        // which calls jfn_cef_layer_set_popup_options.
        self.send_process_message_named("getPopupOptions");
    }

    pub(crate) fn on_popup_size(self: &Arc<Self>, x: i32, y: i32, w: i32, h: i32) {
        {
            let mut p = self.popup.lock();
            p.x = x;
            p.y = y;
            p.w = w;
            p.h = h;
            p.size_received = true;
        }
        self.try_show_popup();
    }

    pub(crate) fn set_popup_options(self: &Arc<Self>, opts: Vec<String>, selected: i32) {
        {
            let mut p = self.popup.lock();
            p.options = opts;
            p.selected_idx = selected;
            p.options_received = true;
        }
        self.try_show_popup();
    }

    fn try_show_popup(self: &Arc<Self>) {
        let (x, y, w, h, opts, selected) = {
            let p = self.popup.lock();
            if !p.visible || !p.size_received || !p.options_received {
                return;
            }
            (p.x, p.y, p.w, p.h, p.options.clone(), p.selected_idx)
        };

        let surface = self.surface_ptr();
        if surface.is_null() {
            return;
        }
        let Some(p) = platform_ops::ops() else { return };

        let inner = Arc::clone(self);
        let req = platform_ops::JfnPopupRequest {
            x,
            y,
            lw: w,
            lh: h,
            options: opts,
            initial_highlight: selected,
            // Fires only on native-menu backends (macOS); compositor
            // backends (Wayland/X11/Windows) drop the closure — CEF
            // dispatches selection itself on click.
            on_selected: Some(Box::new(move |idx| {
                let mut task = DispatchPopupTask::new(inner, idx);
                let _ = post_task(ThreadId::UI, Some(&mut task));
            })),
        };
        p.popup_show(surface, req);
    }

    fn on_deactivated(&self) {
        let was_visible = {
            let mut p = self.popup.lock();
            let was = p.visible;
            if was {
                p.visible = false;
                Self::reset_popup_state(&mut p);
            }
            was
        };
        if !was_visible {
            return;
        }
        let surface = self.surface_ptr();
        if surface.is_null() {
            return;
        }
        if let Some(p) = platform_ops::ops() {
            p.popup_hide(surface);
        }
    }

    fn popup_rect(&self) -> (i32, i32) {
        let p = self.popup.lock();
        (p.w, p.h)
    }

    // ---- lifecycle / reset (slice 5) -------------------------------------

    /// Called by the CEF client impl after browser_ has been assigned and
    /// the CEF-side WasResized + Invalidate kick has fired. Returns 1 when
    /// the caller should close the freshly created browser (PendingReset
    /// path); 0 otherwise. The caller then fires the on_after_created
    /// callback and drains any buffered URL via
    /// `jfn_cef_layer_take_pending_url`.
    fn on_after_created(&self) -> i32 {
        self.has_browser.store(true, Ordering::Release);
        match self.state.load(Ordering::Acquire) {
            STATE_PENDING_RESET => {
                self.state.store(STATE_RECREATING, Ordering::Release);
                1
            }
            STATE_RECREATING => {
                self.state.store(STATE_NORMAL, Ordering::Release);
                0
            }
            _ => 0,
        }
    }

    fn on_before_close(self: &Arc<Self>) {
        self.has_browser.store(false, Ordering::Release);
        if self.pending_internal_reset.swap(false, Ordering::AcqRel) {
            let inner = Arc::clone(self);
            let mut task = ResetCreateTask::new(inner);
            let _ = post_task(ThreadId::UI, Some(&mut task));
        }
    }

    pub(crate) fn create(self: &Arc<Self>, url: &str) {
        self.cef_create_browser(url);
    }

    pub(crate) fn reset(&self) {
        if self.state.load(Ordering::Acquire) != STATE_NORMAL {
            return;
        }
        // One-shot: when OnBeforeClose fires, ResetCreateTask spins up a
        // fresh blank browser. OnBeforeClose runs synchronously from within
        // CEF's destroy chain, so the create must be deferred — calling it
        // inline reenters CEF while WebContents is mid-destroy and crashes
        // inside libcef.
        self.pending_internal_reset.store(true, Ordering::Release);
        if self.has_browser.load(Ordering::Acquire) {
            self.state.store(STATE_RECREATING, Ordering::Release);
            self.cef_close_browser();
        } else {
            // Initial create still in flight. Defer the close to OnAfterCreated.
            self.state.store(STATE_PENDING_RESET, Ordering::Release);
        }
    }

    pub(crate) fn load_url(&self, url: &str) {
        // If a reset is in flight or the initial create hasn't completed,
        // buffer the URL; OnAfterCreated drains it via take_pending_url.
        if self.state.load(Ordering::Acquire) != STATE_NORMAL
            || !self.has_browser.load(Ordering::Acquire)
        {
            *self.pending_url.lock() = url.to_string();
            return;
        }
        self.cef_load_url(url);
    }

    fn take_pending_url(&self) -> Option<String> {
        let mut g = self.pending_url.lock();
        if g.is_empty() {
            None
        } else {
            Some(std::mem::take(&mut *g))
        }
    }

    pub(crate) fn on_fullscreen_mode_change(&self, fullscreen: bool) {
        if let Some(p) = platform_ops::ops() {
            p.set_fullscreen(fullscreen);
        }
    }

    pub(crate) fn on_cursor_change(&self, cursor_type: c_int) {
        if let Some(p) = platform_ops::ops() {
            p.set_cursor(cursor_type);
        }
    }

    pub(crate) fn on_console_message(&self, level: c_int, msg: &str, src: &str, line: c_int) {
        // CEF severities: VERBOSE/DEBUG share LOGSEVERITY_VERBOSE; DEFAULT=0
        // treated as INFO. Numeric values mirror cef_log_severity_t.
        const LOGSEVERITY_VERBOSE: c_int = 1; // and DEBUG
        const LOGSEVERITY_INFO: c_int = 2;
        const LOGSEVERITY_WARNING: c_int = 3;
        const LOGSEVERITY_ERROR: c_int = 4;
        const LOGSEVERITY_DEFAULT: c_int = 0;
        let formatted = format!("{} ({}:{})", msg, src, line);
        let lvl = if level >= LOGSEVERITY_ERROR {
            jfn_logging::LEVEL_ERROR
        } else if level == LOGSEVERITY_WARNING {
            jfn_logging::LEVEL_WARN
        } else if level == LOGSEVERITY_INFO || level == LOGSEVERITY_DEFAULT {
            jfn_logging::LEVEL_INFO
        } else {
            let _ = LOGSEVERITY_VERBOSE;
            jfn_logging::LEVEL_DEBUG
        };
        jfn_logging::log(jfn_logging::CATEGORY_JS, lvl, &formatted);
    }

    pub(crate) fn on_load_end(&self, is_main: bool, code: c_int, url: &str) {
        let formatted = format!(
            "CefLayer::OnLoadEnd name={} main={} code={} url={}",
            self.name_str(),
            if is_main { 1 } else { 0 },
            code,
            url,
        );
        jfn_logging::log(
            jfn_logging::CATEGORY_CEF,
            jfn_logging::LEVEL_INFO,
            &formatted,
        );
        if is_main {
            let _g = self.load_mtx.lock();
            self.loaded.store(true, Ordering::Release);
            self.load_cv.notify_all();
        }
    }

    pub(crate) fn on_load_error(&self, code: c_int, text: &str, url: &str) {
        let formatted = format!(
            "OnLoadError name={} url={} error={} {}",
            self.name_str(),
            url,
            code,
            text,
        );
        jfn_logging::log(
            jfn_logging::CATEGORY_CEF,
            jfn_logging::LEVEL_ERROR,
            &formatted,
        );
    }

    /// OnPreKeyEvent paste intercept. Caller has already matched the
    /// platform paste shortcut. Returns true if a platform clipboard read
    /// was triggered (CEF should swallow the key); false otherwise.
    pub(crate) fn set_visible(&self, visible: bool) {
        let surface = self.surface_ptr();
        if surface.is_null() {
            return;
        }
        if let Some(p) = platform_ops::ops() {
            p.surface_set_visible(surface, visible);
        }
    }

    pub(crate) fn try_paste(self: &Arc<Self>) -> bool {
        let Some(p) = platform_ops::ops() else {
            return false;
        };
        if !p.clipboard_text_supported() {
            return false;
        }
        let inner = Arc::clone(self);
        p.clipboard_read_text_async(Box::new(move |text| {
            if text.is_empty() {
                return;
            }
            let mut task = PasteJsTask::new(inner, text.to_string());
            let _ = post_task(ThreadId::UI, Some(&mut task));
        }));
        true
    }

    // ---- queries / helpers for cef-rs handlers ---------------------------

    pub(crate) fn view_size(&self) -> (i32, i32) {
        (
            self.width.load(Ordering::Acquire),
            self.height.load(Ordering::Acquire),
        )
    }

    pub(crate) fn screen_info_values(&self) -> (f32, i32, i32) {
        let w = self.width.load(Ordering::Acquire);
        let h = self.height.load(Ordering::Acquire);
        let pw = self.physical_w.load(Ordering::Acquire);
        let scale = if pw > 0 && w > 0 {
            pw as f32 / w as f32
        } else {
            1.0
        };
        (scale, w, h)
    }

    pub(crate) fn handle_on_after_created(self: &Arc<Self>, browser: Browser) {
        let formatted = format!("CefLayer::OnAfterCreated name={}", self.name_str());
        jfn_logging::log(
            jfn_logging::CATEGORY_CEF,
            jfn_logging::LEVEL_DEBUG,
            &formatted,
        );
        *self.browser.lock() = Some(browser.clone());
        {
            let _g = self.close_mtx.lock();
            self.closed.store(false, Ordering::Release);
            self.close_cv.notify_all();
        }
        {
            let _g = self.load_mtx.lock();
            self.loaded.store(false, Ordering::Release);
            self.load_cv.notify_all();
        }
        if jfn_shutting_down() {
            if let Some(h) = browser.host() {
                h.close_browser(1);
            }
            return;
        }
        // WasResized fires here, so bump gen so should_present_paint recomputes
        // skip/pump from frame_rate on the first paint.
        self.resize_gen.fetch_add(1, Ordering::AcqRel);
        if let Some(h) = browser.host() {
            h.notify_screen_info_changed();
            h.was_resized();
            h.invalidate(PaintElementType::VIEW);
        }
        self.kick_invalidate_loop();

        // Reset state machine: PendingReset path → close the freshly created
        // browser so the deferred replacement spawns via ResetCreateTask.
        let action = self.on_after_created();
        if action == 1 {
            if let Some(h) = browser.host() {
                h.close_browser(1);
            }
            return;
        }

        // Invoke user-installed created callback with a raw, add-refed
        // CefBrowser pointer.
        let g = self.created_callback.lock();
        if let Some(f) = g.as_ref() {
            unsafe {
                browser.add_ref();
                let raw = ImplBrowser::get_raw(&browser) as *mut c_void;
                f(raw);
            }
        }
        drop(g);

        // Flush any URL buffered while the browser wasn't ready.
        if let Some(url) = self.take_pending_url()
            && let Some(f) = browser.main_frame()
        {
            f.load_url(Some(&CefString::from(url.as_str())));
        }
    }

    /// Block until this layer's browser has closed (`OnBeforeClose` fired).
    /// Callers hold the `Arc<Inner>`, so this is safe even after the owning
    /// `JfnCefLayer` Box is freed during close.
    pub(crate) fn wait_for_close(&self) {
        let mut g = self.close_mtx.lock();
        while !self.closed.load(Ordering::Acquire) {
            self.close_cv.wait(&mut g);
        }
    }

    pub(crate) fn handle_on_before_close(self: &Arc<Self>) {
        *self.browser.lock() = None;
        // Signal the nudge loop to exit so the posted-task Arc clones keeping
        // Rust state alive can drop and the layer can finish destruction.
        self.invalidate_stop.store(true, Ordering::Release);
        {
            let _g = self.close_mtx.lock();
            self.closed.store(true, Ordering::Release);
            self.close_cv.notify_all();
        }
        {
            let _g = self.load_mtx.lock();
            self.loaded.store(true, Ordering::Release);
            self.load_cv.notify_all();
        }
        self.on_before_close();
        // Unified layer cleanup: drop this layer from the Browsers registry
        // BEFORE invoking the user before_close_callback. Order matters —
        // it lets a singleton-style module (e.g. business_about) clear its
        // open-status flag knowing the layer is already gone, preventing
        // a racing re-open from pushing a second layer onto active_stack
        // while the old one is still mid-teardown.
        // `jfn_browsers_remove` no-ops on null and on layers already removed,
        // so it's safe under shutdown's bulk-close path too.
        let lp = self.layer_ptr.swap(std::ptr::null_mut(), Ordering::AcqRel);
        if !lp.is_null() {
            jfn_logging::log(
                jfn_logging::CATEGORY_CEF,
                jfn_logging::LEVEL_DEBUG,
                &format!(
                    "CefLayer::OnBeforeClose name={} -> auto-remove",
                    self.name_str()
                ),
            );
            crate::browsers::jfn_browsers_remove(lp);
        }
        // Take-and-invoke so the callback can install a new slot without
        // destroying its own closure mid-call.
        let slot = self.before_close_callback.lock().take();
        if let Some(f) = slot {
            f();
        }
    }

    pub(crate) fn handle_menu_item_selected(&self, cmd: c_int, browser: Option<&mut Browser>) {
        {
            let mut g = self.pending_menu_callback.lock();
            if let Some(cb) = g.take() {
                cb.cancel();
            }
        }
        let Some(b) = browser else { return };
        let frame = b.focused_frame().or_else(|| b.main_frame());
        let menu_back = MenuId::BACK.get_raw() as c_int;
        let menu_forward = MenuId::FORWARD.get_raw() as c_int;
        let menu_reload = MenuId::RELOAD.get_raw() as c_int;
        let menu_reload_nocache = MenuId::RELOAD_NOCACHE.get_raw() as c_int;
        let menu_stop = MenuId::STOPLOAD.get_raw() as c_int;
        let menu_undo = MenuId::UNDO.get_raw() as c_int;
        let menu_redo = MenuId::REDO.get_raw() as c_int;
        let menu_cut = MenuId::CUT.get_raw() as c_int;
        let menu_copy = MenuId::COPY.get_raw() as c_int;
        let menu_paste = MenuId::PASTE.get_raw() as c_int;
        let menu_select_all = MenuId::SELECT_ALL.get_raw() as c_int;
        if cmd == menu_back {
            b.go_back();
        } else if cmd == menu_forward {
            b.go_forward();
        } else if cmd == menu_reload {
            b.reload();
        } else if cmd == menu_reload_nocache {
            b.reload_ignore_cache();
        } else if cmd == menu_stop {
            b.stop_load();
        } else if cmd == menu_undo {
            if let Some(f) = frame {
                f.undo()
            }
        } else if cmd == menu_redo {
            if let Some(f) = frame {
                f.redo()
            }
        } else if cmd == menu_cut {
            if let Some(f) = frame {
                f.cut()
            }
        } else if cmd == menu_copy {
            if let Some(f) = frame {
                f.copy()
            }
        } else if cmd == menu_paste {
            if let Some(f) = frame {
                f.paste()
            }
        } else if cmd == menu_select_all {
            if let Some(f) = frame {
                f.select_all()
            }
        } else {
            self.invoke_context_menu_dispatcher(cmd);
        }
    }

    pub(crate) fn handle_menu_dismissed(&self) {
        let mut g = self.pending_menu_callback.lock();
        if let Some(cb) = g.take() {
            cb.cancel();
        }
    }

    pub(crate) fn store_pending_menu_callback(&self, cb: RunContextMenuCallback) {
        let mut g = self.pending_menu_callback.lock();
        if let Some(prev) = g.take() {
            prev.cancel();
        }
        *g = Some(cb);
    }

    pub(crate) fn invoke_message_handler(
        &self,
        name: &str,
        args: *mut c_void,
        browser: *mut c_void,
    ) -> bool {
        let g = self.message_handler.lock();
        g.as_ref().map(|f| f(name, args, browser)).unwrap_or(false)
    }

    pub(crate) fn has_context_menu_builder(&self) -> bool {
        self.context_menu_builder.lock().is_some()
    }

    pub(crate) fn invoke_context_menu_builder(&self, menu_model_raw: *mut c_void) {
        let g = self.context_menu_builder.lock();
        if let Some(f) = g.as_ref() {
            f(menu_model_raw);
        }
    }

    fn invoke_context_menu_dispatcher(&self, command_id: c_int) -> bool {
        let g = self.context_menu_dispatcher.lock();
        g.as_ref().map(|f| f(command_id)).unwrap_or(false)
    }

    pub(crate) fn on_before_popup(&self, url: &str) -> bool {
        // Leading '-' guard blocks argv-style option smuggling into xdg-open.
        if url.is_empty() || url.starts_with('-') {
            return true;
        }
        if let Some(p) = platform_ops::ops() {
            p.open_external_url(url);
        }
        true
    }

    // ---- paint dispatch --------------------------------------------------

    pub(crate) fn on_paint(
        &self,
        is_popup: bool,
        dirty: *const platform_ops::JfnRect,
        n: usize,
        buffer: *const c_void,
        w: i32,
        h: i32,
    ) {
        let surface = self.surface_ptr();
        if surface.is_null() {
            return;
        }
        let Some(p) = platform_ops::ops() else { return };
        if is_popup {
            let (pw, ph) = self.popup_rect();
            p.popup_present_software(surface, buffer, w, h, pw, ph);
            return;
        }
        if !self.should_present_paint() {
            return;
        }
        // CEF hands the dirty rects as a raw `RectList` pointer; reborrow as a
        // slice here, the single boundary point, so the platform ABI stays safe.
        let dirty: &[platform_ops::JfnRect] = if dirty.is_null() || n == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(dirty, n) }
        };
        p.surface_present_software(surface, dirty, buffer, w, h);
    }

    pub(crate) fn on_accelerated_paint(&self, is_popup: bool, info: *const c_void) {
        let surface = self.surface_ptr();
        if surface.is_null() || info.is_null() {
            return;
        }
        let Some(p) = platform_ops::ops() else { return };
        if is_popup {
            let (pw, ph) = self.popup_rect();
            p.popup_present(surface, info, pw, ph);
            return;
        }
        if !self.should_present_paint() {
            return;
        }
        p.surface_present(surface, info);
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
            let hz = jfn_playback_display_hz();
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

wrap_task! {
    struct DispatchPopupTask {
        inner: Arc<Inner>,
        index: i32,
    }
    impl Task {
        fn execute(&self) {
            self.inner.dispatch_popup_selection(self.index);
        }
    }
}

wrap_task! {
    struct ResetCreateTask {
        inner: Arc<Inner>,
    }
    impl Task {
        fn execute(&self) {
            // CefShutdown drains pending tasks; creating a browser here would
            // race with the shutdown teardown and cause a hang.
            if jfn_shutting_down() {
                return;
            }
            self.inner.create("");
        }
    }
}

wrap_task! {
    struct PasteJsTask {
        inner: Arc<Inner>,
        text: String,
    }
    impl Task {
        fn execute(&self) {
            let escaped = serde_json::to_string(&self.text).unwrap_or_else(|_| "\"\"".to_string());
            let js = format!("document.execCommand('insertText',false,{});", escaped);
            self.inner.exec_js_focused(&js);
        }
    }
}

type CloseCollectTx = Arc<Mutex<Option<SyncSender<Vec<Arc<Inner>>>>>>;

wrap_task! {
    struct CloseAndCollectTask {
        tx: CloseCollectTx,
    }
    impl Task {
        fn execute(&self) {
            // Single snapshot: take Arc<Inner> + force-close every layer
            // under one INSTANCE lock, on TID_UI. Holding the lock across
            // close_browser_force is safe — that call only schedules close;
            // OnBeforeClose → jfn_browsers_remove fires later on TID_UI
            // after this task unwinds, so no reentrant INSTANCE.lock().
            let inners = crate::browsers::jfn_browsers_close_and_snapshot();
            if let Some(tx) = self.tx.lock().take() {
                let _ = tx.send(inners);
            }
        }
    }
}

/// Post a single-snapshot close-and-collect onto TID_UI. The posted task
/// closes every layer's browser and ships the corresponding `Arc<Inner>`
/// list back via `tx`, so the caller can wait on exactly the set that was
/// closed (no second-snapshot race). Asserts the post: this site is
/// load-bearing for shutdown — silent post loss = process hang.
pub(crate) fn jfn_cef_post_close_and_collect(tx: SyncSender<Vec<Arc<Inner>>>) {
    let mut task = CloseAndCollectTask::new(Arc::new(Mutex::new(Some(tx))));
    assert!(
        post_task(ThreadId::UI, Some(&mut task)) != 0,
        "TID_UI post during shutdown — CEF UI thread invariant broken"
    );
}

// ---------------------------------------------------------------------------
// Pub Rust API: in-process callers install closures directly. Pass `None`
// to clear; the previously installed closure is dropped.
// ---------------------------------------------------------------------------
impl JfnCefLayer {
    pub fn set_message_handler_rust(&self, f: Option<Box<MessageFn>>) {
        *self.inner.message_handler.lock() = f;
    }
    pub fn set_created_callback_rust(&self, f: Option<Box<CreatedFn>>) {
        *self.inner.created_callback.lock() = f;
    }
    pub fn set_before_close_callback_rust(&self, f: Option<Box<BeforeCloseFn>>) {
        *self.inner.before_close_callback.lock() = f;
    }
    pub fn set_context_menu_builder_rust(&self, f: Option<Box<ContextBuilderFn>>) {
        *self.inner.context_menu_builder.lock() = f;
    }
    pub fn set_context_menu_dispatcher_rust(&self, f: Option<Box<ContextDispatcherFn>>) {
        *self.inner.context_menu_dispatcher.lock() = f;
    }
}
