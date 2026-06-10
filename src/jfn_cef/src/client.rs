//! CefLayer state.
//!
//! Holds the small bits of CefLayer state plus the resize-debounce and the
//! per-layer CEF browser ops dispatch that schedules `WasResized`,
//! `NotifyScreenInfoChanged`, `Invalidate`, `SetWindowlessFrameRate`,
//! `SendExternalBeginFrame`, and `ExecuteJavaScript` calls on TID_UI.
//!
//! Lifetime model: the FFI handle is `Box<JfnCefLayer>` (raw pointer owned
//! by the caller). Internal state lives in an `Arc<Inner>` so posted CEF
//! tasks can keep a clone alive past `jfn_cef_layer_free`. CefLayer
//! destructor clears `cef_ops` first, so any in-flight task that does
//! eventually run sees `None` and exits.

use cef::{Browser, RunContextMenuCallback};
use parking_lot::{Condvar, Mutex};
use std::os::raw::{c_int, c_void};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicPtr, Ordering};
use std::sync::{Arc, OnceLock};
use std::time::Instant;

use crate::ipc::BrowserMessage;
use crate::platform_ops;
use crate::sink_routing::Handle;

use crate::paint_scheduler::{PaintMode, PaintScheduler};

mod browser_ops;
mod callbacks;
mod events;
mod ffi;
mod lifecycle;
mod paint;
mod popup;
mod resize;
mod tasks;
pub(crate) use ffi::*;
pub use ffi::{jfn_cef_layer_create, jfn_cef_layer_wait_for_load};
pub(crate) use tasks::{jfn_cef_post_close_and_collect, jfn_cef_post_set_hidden_all};

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
static PAINT_MODE: OnceLock<PaintMode> = OnceLock::new();

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
    // Pending RunContextMenuCallback — held while a context menu is open.
    pending_menu_callback: Mutex<Option<RunContextMenuCallback>>,
    // Selection callback parked by the JS-rendered context-menu backend;
    // fired by the menuItemSelected / menuDismissed IPC (-1 = dismissed).
    pending_menu_on_selected: Mutex<Option<jfn_platform_abi::MenuSelectionFn>>,
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

    paint_scheduler: PaintScheduler,

    // frame rate (slice 3): configured and last applied
    pub(crate) frame_rate: AtomicI32,
    current_frame_rate: AtomicI32,

    // resize-debounce (slice 3)
    resize_scheduled: AtomicBool,
    last_was_resized_ns: AtomicI64,

    // popup state (slice 4). Owned 1:1 with the platform surface; each
    // CefLayer owns its popup on the platform side. Two-phase reveal: rect
    // arrives via OnPopupSize, options via the "popupOptions" renderer IPC;
    // try_show_popup fires when popup_visible + size_received + options_received.
    popup: Mutex<PopupState>,
    dropdown: &'static dyn jfn_platform_abi::DropdownBackend,
    pub(crate) context_menu: &'static dyn jfn_platform_abi::ContextMenuBackend,

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

    cursor_handle: OnceLock<Handle>,
}

// Typed closure signatures stored in each callback slot. `*mut c_void` args
// stay raw because callers may want to receive cef-rs handles or C++
// CefRefPtr objects depending on which side installed the handler.
pub(crate) type MessageFn = dyn Fn(BrowserMessage) -> bool + Send + Sync;
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
    // Option indices an arrow key can land on (disabled/optgroup-disabled
    // excluded). Used to drive CEF's own popup to the chosen row.
    selectable: Vec<i32>,
    // Bottom-left corner of the <select> element in view coordinates.
    anchor: Option<(i32, i32)>,
    size_received: bool,
    options_received: bool,
}

// SAFETY: surface is a C++ pointer treated as opaque; only handed back to
// the platform vtable on TID_UI.
unsafe impl Send for Inner {}
unsafe impl Sync for Inner {}

impl Inner {
    fn new() -> Arc<Self> {
        let paint_scheduler = PAINT_MODE
            .get_or_init(|| PaintMode::new(false))
            .make_scheduler();
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
            pending_menu_on_selected: Mutex::new(None),
            injection_kind: Mutex::new(String::new()),
            surface: Mutex::new(std::ptr::null_mut()),
            width: AtomicI32::new(0),
            height: AtomicI32::new(0),
            physical_w: AtomicI32::new(0),
            physical_h: AtomicI32::new(0),
            paint_scheduler,
            frame_rate: AtomicI32::new(0),
            current_frame_rate: AtomicI32::new(0),
            resize_scheduled: AtomicBool::new(false),
            last_was_resized_ns: AtomicI64::new(0),
            popup: Mutex::new(PopupState {
                selected_idx: -1,
                ..PopupState::default()
            }),
            dropdown: jfn_platform_abi::get().dropdown_backend(),
            context_menu: jfn_platform_abi::get().context_menu_backend(),
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
            cursor_handle: OnceLock::new(),
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

    pub(crate) fn set_cursor_handle(&self, handle: Handle) {
        let _ = self.cursor_handle.set(handle);
    }

    pub(crate) fn cursor_handle(&self) -> Option<Handle> {
        self.cursor_handle.get().copied()
    }

    fn surface_ptr(&self) -> *mut c_void {
        *self.surface.lock()
    }
}

pub(crate) fn now_ns() -> i64 {
    static ORIGIN: OnceLock<Instant> = OnceLock::new();
    Instant::now()
        .duration_since(*ORIGIN.get_or_init(Instant::now))
        .as_nanos() as i64
}

// ---------------------------------------------------------------------------
// Pub Rust API: in-process callers install closures directly. Pass `None`
// to clear; the previously installed closure is dropped.
// ---------------------------------------------------------------------------
impl JfnCefLayer {
    pub(crate) fn set_message_handler_rust(&self, f: Option<Box<MessageFn>>) {
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
