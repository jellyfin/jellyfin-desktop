//! `Platform` trait + global handle held by `jfn_app_main`.
//!
//! Each backend crate (`jfn-wayland`, `jfn-x11`, `jfn-macos`, `jfn-windows`)
//! returns a concrete type implementing this trait via its
//! `make_*_platform()` factory. The binary installs the chosen backend into
//! the [`OnceLock`] below via [`install`] / [`get`].
//!
//! `JfnRect` stays `#[repr(C)]` because CEF's `OnAcceleratedPaint` accel-paint
//! info hands it across the C ABI surface; the popup request and other
//! payloads are plain Rust.

#![allow(non_snake_case)]

use std::ffi::{CString, c_char, c_int, c_void};
use std::sync::OnceLock;

pub mod geometry;

pub use geometry::{
    BootGeometry, LogicalSize, PhysicalSize, Scale, SurfaceSize, WindowGeometry, WindowPos,
};

// =====================================================================
// Main-thread park (non-macOS default for run_main_loop/wake_main_loop)
// =====================================================================
//
// Non-macOS backends have no native run loop to block the process main
// thread on. The default `Platform::run_main_loop` parks here until the
// shutdown manager calls `wake_main_loop`, at which point main runs the
// teardown tail. A latching `bool` + `Condvar` is enough — it's a single
// dedicated blocking wait (not a `poll()` multiplexer), so no fd is needed
// and there's no `playback`-crate dependency. macOS overrides both methods
// (`[NSApp run]` / stop-NSApp) and never touches this.

struct MainPark {
    woken: std::sync::Mutex<bool>,
    cv: std::sync::Condvar,
}

static MAIN_PARK: MainPark = MainPark {
    woken: std::sync::Mutex::new(false),
    cv: std::sync::Condvar::new(),
};

/// Block until [`main_park_signal`] is called. Returns immediately if the
/// signal already fired (latched), so a wake racing ahead of the wait is
/// not lost.
pub fn main_park_wait() {
    let mut woken = MAIN_PARK
        .woken
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    while !*woken {
        woken = MAIN_PARK
            .cv
            .wait(woken)
            .unwrap_or_else(std::sync::PoisonError::into_inner);
    }
}

/// Release [`main_park_wait`]. Idempotent and safe from any thread.
pub fn main_park_signal() {
    let mut woken = MAIN_PARK
        .woken
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    *woken = true;
    MAIN_PARK.cv.notify_all();
}

/// Fixed `cef_cursor_type_t` shapes. `CT_CUSTOM` (a bitmap) and the `CT_DND_*`
/// cursors are excluded — listing them here would map a non-fixed cursor to a
/// fixed shape.
pub mod cursor {
    use cef::sys::cef_cursor_type_t as ct;

    macro_rules! cursor_shape {
        ($($variant:ident = $ct:ident),* $(,)?) => {
            #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
            #[repr(i32)]
            pub enum CursorShape {
                $($variant = ct::$ct as i32,)*
            }

            impl CursorShape {
                pub fn from_cef(raw: i32) -> Option<Self> {
                    $(if raw == ct::$ct as i32 { return Some(Self::$variant); })*
                    None
                }

                pub const fn as_raw(self) -> i32 {
                    self as i32
                }
            }
        };
    }

    cursor_shape! {
        Pointer = CT_POINTER,
        Cross = CT_CROSS,
        Hand = CT_HAND,
        IBeam = CT_IBEAM,
        Wait = CT_WAIT,
        Help = CT_HELP,
        EastResize = CT_EASTRESIZE,
        NorthResize = CT_NORTHRESIZE,
        NorthEastResize = CT_NORTHEASTRESIZE,
        NorthWestResize = CT_NORTHWESTRESIZE,
        SouthResize = CT_SOUTHRESIZE,
        SouthEastResize = CT_SOUTHEASTRESIZE,
        SouthWestResize = CT_SOUTHWESTRESIZE,
        WestResize = CT_WESTRESIZE,
        NorthSouthResize = CT_NORTHSOUTHRESIZE,
        EastWestResize = CT_EASTWESTRESIZE,
        NorthEastSouthWestResize = CT_NORTHEASTSOUTHWESTRESIZE,
        NorthWestSouthEastResize = CT_NORTHWESTSOUTHEASTRESIZE,
        ColumnResize = CT_COLUMNRESIZE,
        RowResize = CT_ROWRESIZE,
        MiddlePanning = CT_MIDDLEPANNING,
        EastPanning = CT_EASTPANNING,
        NorthPanning = CT_NORTHPANNING,
        NorthEastPanning = CT_NORTHEASTPANNING,
        NorthWestPanning = CT_NORTHWESTPANNING,
        SouthPanning = CT_SOUTHPANNING,
        SouthEastPanning = CT_SOUTHEASTPANNING,
        SouthWestPanning = CT_SOUTHWESTPANNING,
        WestPanning = CT_WESTPANNING,
        Move = CT_MOVE,
        VerticalText = CT_VERTICALTEXT,
        Cell = CT_CELL,
        ContextMenu = CT_CONTEXTMENU,
        Alias = CT_ALIAS,
        Progress = CT_PROGRESS,
        NoDrop = CT_NODROP,
        Copy = CT_COPY,
        None = CT_NONE,
        NotAllowed = CT_NOTALLOWED,
        ZoomIn = CT_ZOOMIN,
        ZoomOut = CT_ZOOMOUT,
        Grab = CT_GRAB,
        Grabbing = CT_GRABBING,
        MiddlePanningVertical = CT_MIDDLE_PANNING_VERTICAL,
        MiddlePanningHorizontal = CT_MIDDLE_PANNING_HORIZONTAL,
    }
}

/// Canonical `cef_event_flags_t` modifier bits — the single source of truth
/// for the CEF `EVENTFLAG_*` masks that flow through key/mouse dispatch.
/// Derived from the generated CEF bindings (a newtype with associated
/// constants) so backends import these instead of hand-copying bit shifts
/// that can silently drift. Typed `u32` to match the dispatch ABI.
pub mod event_flags {
    use cef::sys::cef_event_flags_t as ef;

    macro_rules! flag_consts {
        ($($name:ident),* $(,)?) => {
            $(pub const $name: u32 = ef::$name.0 as u32;)*
        };
    }

    flag_consts! {
        EVENTFLAG_CAPS_LOCK_ON, EVENTFLAG_SHIFT_DOWN, EVENTFLAG_CONTROL_DOWN,
        EVENTFLAG_ALT_DOWN, EVENTFLAG_LEFT_MOUSE_BUTTON, EVENTFLAG_MIDDLE_MOUSE_BUTTON,
        EVENTFLAG_RIGHT_MOUSE_BUTTON, EVENTFLAG_COMMAND_DOWN, EVENTFLAG_NUM_LOCK_ON,
        EVENTFLAG_IS_KEY_PAD, EVENTFLAG_IS_LEFT, EVENTFLAG_IS_RIGHT, EVENTFLAG_ALTGR_DOWN,
        EVENTFLAG_IS_REPEAT, EVENTFLAG_PRECISION_SCROLLING_DELTA, EVENTFLAG_SCROLL_BY_PAGE,
    }
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum DisplayBackend {
    Wayland,
    X11,
    Windows,
    MacOS,
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum WindowDecorations {
    /// Client-side: the app draws its own titlebar in-page.
    Csd,
    Server,
    ServerThemed,
}

impl WindowDecorations {
    /// Wire/persistence contract: settings.json, the JS↔Rust IPC, and the web
    /// settings UI all speak these literals.
    pub fn as_str(self) -> &'static str {
        match self {
            WindowDecorations::Csd => "csd",
            WindowDecorations::Server => "server",
            WindowDecorations::ServerThemed => "serverThemed",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "csd" => Some(WindowDecorations::Csd),
            "server" => Some(WindowDecorations::Server),
            "serverThemed" => Some(WindowDecorations::ServerThemed),
            _ => None,
        }
    }
}

#[repr(C)]
pub struct JfnRect {
    pub x: c_int,
    pub y: c_int,
    pub w: c_int,
    pub h: c_int,
}

pub struct JfnPopupRequest {
    pub x: c_int,
    pub y: c_int,
    pub lw: c_int,
    pub lh: c_int,
    pub options: Vec<String>,
    pub initial_highlight: c_int,
    /// Fires on the platform backend's thread when the user picks an
    /// option (or `-1` for cancel). Native-menu backends (macOS / Wayland)
    /// call it; compositor-rendered backends (X11 / Windows) drop the closure
    /// without firing — CEF dispatches selection itself.
    pub on_selected: Option<Box<dyn FnOnce(c_int) + Send>>,
}

pub struct JfnMenuItem {
    pub id: c_int,
    pub label: String,
    pub enabled: bool,
    pub separator: bool,
}

pub struct JfnContextMenuRequest {
    /// Logical (CEF view) coordinates of the click, not physical pixels.
    pub x: c_int,
    pub y: c_int,
    pub items: Vec<JfnMenuItem>,
    /// Called with the chosen item id, or `-1` when the menu is dismissed.
    pub on_selected: Option<Box<dyn FnOnce(c_int) + Send>>,
}

/// Idle-inhibit level.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum IdleInhibitLevel {
    None,
    System,
    Display,
}

/// Backend-allocated per-surface handle. Backends define the layout
/// in-crate; callers only ever hold the raw pointer.
pub type SurfaceHandle = *mut c_void;

/// Process-wide platform handle. Optional methods have no-op defaults so
/// backends only override what they care about.
///
/// All methods take `&self` — backends keep their own interior mutability
/// (`Mutex`, `AtomicBool`, etc) where they need it.
pub trait Platform: Send + Sync {
    fn display(&self) -> DisplayBackend;

    fn default_window_decorations(&self) -> WindowDecorations;

    fn resolve_window_decorations(
        &self,
        configured: Option<WindowDecorations>,
    ) -> WindowDecorations {
        configured.unwrap_or_else(|| self.default_window_decorations())
    }

    fn early_init(&self) {}
    /// `mpv` is the opaque libmpv `mpv_handle` — a raw C handle, stays raw.
    fn init(&self, mpv: *mut c_void) -> bool {
        let _ = mpv;
        true
    }
    fn cleanup(&self) {}
    fn post_window_cleanup(&self) {}

    // Per-surface
    fn alloc_surface(&self) -> SurfaceHandle {
        std::ptr::null_mut()
    }
    fn free_surface(&self, _s: SurfaceHandle) {}
    /// `_info` is CEF's `OnAcceleratedPaint` accel-paint info — a raw C
    /// pointer that crosses the CEF ABI, so it stays raw.
    fn surface_present(&self, _s: SurfaceHandle, _info: *const c_void) -> bool {
        false
    }
    /// `_buffer` is CEF's `OnPaint` software paint buffer — a raw C pointer
    /// that crosses the CEF ABI, so it stays raw.
    fn surface_present_software(
        &self,
        _s: SurfaceHandle,
        _dirty: &[JfnRect],
        _buffer: *const c_void,
        _w: c_int,
        _h: c_int,
    ) -> bool {
        false
    }
    fn surface_resize(&self, _s: SurfaceHandle, _size: SurfaceSize) {}
    fn surface_set_visible(&self, _s: SurfaceHandle, _visible: bool) {}
    fn restack(&self, _ordered: &[SurfaceHandle]) {}

    // Popup
    fn popup_show(&self, _s: SurfaceHandle, _req: JfnPopupRequest) {}

    fn context_menu_show(&self, _s: SurfaceHandle, _req: JfnContextMenuRequest) {}
    fn popup_hide(&self, _s: SurfaceHandle) {}
    fn popup_present(&self, _s: SurfaceHandle, _info: *const c_void, _lw: c_int, _lh: c_int) {}
    fn popup_present_software(
        &self,
        _s: SurfaceHandle,
        _buffer: *const c_void,
        _pw: c_int,
        _ph: c_int,
        _lw: c_int,
        _lh: c_int,
    ) {
    }

    // Fullscreen
    fn set_fullscreen(&self, _v: bool) {}
    fn toggle_fullscreen(&self) {}

    // Window controls for client-side decorations. Default no-ops cover
    // backends without CSD (X11 WMs / macOS / Windows draw their own).
    fn window_minimize(&self) {}
    fn window_toggle_maximize(&self) {}
    /// Begin an interactive, compositor-driven window move. Must be called in
    /// response to a pointer button press on the titlebar drag region.
    fn window_start_move(&self) {}
    /// Begin an interactive, compositor-driven resize from the given edge.
    /// `edge` uses xdg_toplevel resize-edge values (1=top, 2=bottom, 4=left,
    /// 8=right, corners are the ORs, e.g. 5=top-left).
    fn window_start_resize(&self, _edge: c_int) {}

    // Transition
    fn begin_transition(&self) {}
    fn end_transition(&self) {}
    fn in_transition(&self) -> bool {
        false
    }
    fn set_expected_size(&self, _w: c_int, _h: c_int) {}

    fn get_scale(&self) -> f32 {
        1.0
    }
    fn get_display_scale(&self, _x: c_int, _y: c_int) -> f32 {
        1.0
    }

    /// Seed the window owner with the restored boot geometry. Backends that
    /// own their toplevel (Wayland) size it here; mpv-backed backends rely on
    /// mpv's `--geometry` instead and keep the no-op default.
    fn apply_boot_geometry(&self, _g: &BootGeometry) {}

    /// Current window position, or `None` if it can't be determined.
    fn query_window_position(&self) -> Option<WindowPos> {
        None
    }
    /// Clamp saved geometry to stay on-screen. Backends that don't constrain
    /// geometry return `g` unchanged (the default).
    fn clamp_window_geometry(&self, g: WindowGeometry) -> WindowGeometry {
        g
    }

    fn pump(&self) {}
    /// Block the process main thread until [`wake_main_loop`] is called.
    /// Default parks on the process-wide [`main_park_wait`]; macOS overrides
    /// with `[NSApp run]`.
    fn run_main_loop(&self) {
        main_park_wait();
    }
    /// Release [`run_main_loop`] so main can run the teardown tail. Safe from
    /// any thread. Default signals [`main_park_signal`]; macOS overrides to
    /// stop the NSApp loop.
    fn wake_main_loop(&self) {
        main_park_signal();
    }

    fn set_cursor(&self, _shape: cursor::CursorShape) {}
    fn set_idle_inhibit(&self, _level: IdleInhibitLevel) {}
    fn set_theme_color(&self, _rgb: u32) {}

    fn shared_texture_supported(&self) -> bool {
        true
    }
    /// Set during init by Wayland backend (dmabuf probe) when GPU lacks the
    /// shared-texture path.
    fn set_shared_texture_unsupported(&self) {}

    /// CEF ozone platform name (e.g. "wayland" / "x11"). Stored in the
    /// shared `OZONE_PLATFORM` cell below — the default impls cover every
    /// backend; no backend needs to override these.
    fn cef_ozone_platform(&self) -> *const c_char {
        ozone_platform_get()
    }
    fn set_cef_ozone_platform(&self, name: &str) {
        ozone_platform_set(name);
    }

    /// Whether [`clipboard_read_text_async`] will actually invoke the
    /// backend clipboard. Wayland clears this in `wl_init` when no data
    /// device manager is present; the menu Paste path uses it to decide
    /// between native OS read vs CEF `frame.Paste()`.
    fn clipboard_text_supported(&self) -> bool {
        true
    }

    fn clipboard_read_text_async(&self, on_done: Box<dyn FnOnce(&str) + Send>) {
        // No backend support — invoke with empty text synchronously.
        on_done("");
    }
    /// Disable subsequent clipboard reads (set by Wayland when no data
    /// device manager is available).
    fn clear_clipboard_handler(&self) {}

    fn open_external_url(&self, _url: &str) {}
}

// =====================================================================
// Process-wide handle
// =====================================================================

// `OnceLock<Box<dyn Platform>>` doesn't give us a stable `'static` reference
// shape that's ergonomic for the existing `unsafe extern "C"` thunks below;
// store a raw fat pointer instead. Set exactly once during boot.
static PLATFORM: OnceLock<&'static dyn Platform> = OnceLock::new();

/// Install the platform backend. Must be called exactly once during boot,
/// before any other code dispatches through [`get`]. Panics if called
/// twice — there is no "swap backend at runtime" path.
#[allow(clippy::expect_used)] // boot invariant: install exactly once
pub fn install(p: Box<dyn Platform>) {
    let leaked: &'static dyn Platform = Box::leak(p);
    PLATFORM
        .set(leaked)
        .map_err(|_| ())
        .expect("install() called twice");
}

// CEF ozone platform name (NUL-terminated). Set once by jfn_app_main
// before `Platform::init`; read by the Wayland dmabuf probe.
static OZONE_PLATFORM: OnceLock<CString> = OnceLock::new();

fn ozone_platform_get() -> *const c_char {
    match OZONE_PLATFORM.get() {
        Some(cs) => cs.as_ptr(),
        None => c"".as_ptr(),
    }
}

fn ozone_platform_set(name: &str) {
    let cs = CString::new(name).unwrap_or_default();
    // Best-effort: silently ignore a second set so callers can stay simple.
    let _ = OZONE_PLATFORM.set(cs);
}

/// Returns the installed platform backend. Panics if [`install`] hasn't
/// been called yet — every call site is post-boot.
#[allow(clippy::expect_used)] // every call site is post-boot
pub fn get() -> &'static dyn Platform {
    *PLATFORM
        .get()
        .expect("jfn_platform_abi::get() called before install()")
}

/// Like [`get`] but returns `None` before install. Used by jfn_cef's
/// `OnConsoleMessage` and similar paths that may fire during early CEF
/// helper-process boot when no platform is installed.
pub fn try_get() -> Option<&'static dyn Platform> {
    PLATFORM.get().copied()
}

// =====================================================================
// Browser bridge
// =====================================================================
//
// Lets crates that can't depend on jfn_cef (input, macos) forward events
// to whichever CEF layer is currently active. jfn_cef installs the impl
// at boot; the trait methods resolve the active layer internally so
// callers never see a JfnCefLayer pointer.

pub trait BrowserBridge: Send + Sync {
    #[allow(clippy::too_many_arguments)] // mirrors CEF's KeyEvent layout 1:1
    fn send_key_event(
        &self,
        type_: c_int,
        modifiers: u32,
        windows_key_code: c_int,
        native_key_code: c_int,
        is_system_key: bool,
        character: u16,
        unmodified_character: u16,
    );
    fn send_mouse_click(
        &self,
        x: c_int,
        y: c_int,
        modifiers: u32,
        button: c_int,
        mouse_up: bool,
        click_count: c_int,
    );
    fn send_mouse_move(&self, x: i32, y: i32, modifiers: u32, leave: bool);
    fn send_mouse_wheel(&self, x: c_int, y: c_int, modifiers: u32, delta_x: c_int, delta_y: c_int);
    fn set_focus(&self, focus: bool);
    fn navigate_history(&self, forward: bool);
    fn undo(&self);
    fn redo(&self);
    fn cut(&self);
    fn copy(&self);
    fn paste(&self);
    fn select_all(&self);
    /// True if a layer is currently active. Cheap check used by callers
    /// that want to early-out before building an event payload.
    fn has_active(&self) -> bool;
}

static BROWSER_BRIDGE: OnceLock<&'static dyn BrowserBridge> = OnceLock::new();

#[allow(clippy::expect_used)] // boot invariant: install exactly once
pub fn install_browser_bridge(b: Box<dyn BrowserBridge>) {
    let leaked: &'static dyn BrowserBridge = Box::leak(b);
    BROWSER_BRIDGE
        .set(leaked)
        .map_err(|_| ())
        .expect("install_browser_bridge called twice");
}

pub fn browser_bridge() -> Option<&'static dyn BrowserBridge> {
    BROWSER_BRIDGE.get().copied()
}
