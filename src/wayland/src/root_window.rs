use std::ffi::c_void;
use std::num::{NonZeroI32, NonZeroU64};
use std::os::fd::{AsFd, AsRawFd};
use std::sync::Arc;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU32, AtomicU64, Ordering};
use std::thread::{self, JoinHandle};

use parking_lot::Mutex;

use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::{
    wl_buffer::WlBuffer, wl_compositor::WlCompositor, wl_registry::WlRegistry, wl_seat::WlSeat,
    wl_shm::WlShm, wl_shm_pool::WlShmPool, wl_surface::WlSurface,
};
use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle};
use wayland_protocols::wp::fractional_scale::v1::client::{
    wp_fractional_scale_manager_v1::WpFractionalScaleManagerV1,
    wp_fractional_scale_v1::{self, WpFractionalScaleV1},
};
use wayland_protocols::wp::viewporter::client::{
    wp_viewport::WpViewport, wp_viewporter::WpViewporter,
};
use wayland_protocols::xdg::decoration::zv1::client::{
    zxdg_decoration_manager_v1::ZxdgDecorationManagerV1,
    zxdg_toplevel_decoration_v1::{self, Mode as DecorationMode, ZxdgToplevelDecorationV1},
};
use wayland_protocols::xdg::shell::client::{
    xdg_popup::{self, XdgPopup},
    xdg_positioner::{Anchor, ConstraintAdjustment, Gravity, XdgPositioner},
    xdg_surface::{self, XdgSurface},
    xdg_toplevel::{self, XdgToplevel},
    xdg_wm_base::{self, XdgWmBase},
};
#[cfg(feature = "kde-palette")]
use wayland_protocols_plasma::server_decoration_palette::client::{
    org_kde_kwin_server_decoration_palette::OrgKdeKwinServerDecorationPalette,
    org_kde_kwin_server_decoration_palette_manager::OrgKdeKwinServerDecorationPaletteManager,
};

use jfn_platform_abi::WindowDecorations;

const APP_ID: &str = "net.nullsum.JelliumDesktop";
const TITLE: &str = "Jellium Desktop";

// Background behind the video/overlay, matching kBgColor (0x101010).
const BG: [u8; 3] = [0x10, 0x10, 0x10];

const DEFAULT_W: i32 = 1280;
const DEFAULT_H: i32 = 720;

const STATE_MAXIMIZED: u32 = 1;
const STATE_FULLSCREEN: u32 = 2;
const STATE_SUSPENDED: u32 = 9;
// xdg_toplevel tiled edges (5..=8); any of them means compositor-tiled.
const STATE_TILED_LEFT: u32 = 5;
const STATE_TILED_RIGHT: u32 = 6;
const STATE_TILED_TOP: u32 = 7;
const STATE_TILED_BOTTOM: u32 = 8;

static WANT_CSD: AtomicBool = AtomicBool::new(false);

pub(crate) fn set_decorations(mode: WindowDecorations) {
    WANT_CSD.store(matches!(mode, WindowDecorations::Csd), Ordering::Release);
}

static BOOT_W: AtomicU32 = AtomicU32::new(DEFAULT_W as u32);
static BOOT_H: AtomicU32 = AtomicU32::new(DEFAULT_H as u32);
static BOOT_MAX: AtomicBool = AtomicBool::new(false);

pub(crate) fn set_boot_geometry(w: i32, h: i32, maximized: bool) {
    if let Some(size) = crate::window_state::WindowSize::new(w, h) {
        BOOT_W.store(size.w() as u32, Ordering::Release);
        BOOT_H.store(size.h() as u32, Ordering::Release);
    }
    BOOT_MAX.store(maximized, Ordering::Release);
}

fn boot_geometry() -> (i32, i32, bool) {
    (
        BOOT_W.load(Ordering::Acquire) as i32,
        BOOT_H.load(Ordering::Acquire) as i32,
        BOOT_MAX.load(Ordering::Acquire),
    )
}

struct RootState {
    conn: Connection,
    qh: QueueHandle<RootState>,
    surface: WlSurface,
    xdg_surface: XdgSurface,
    #[allow(dead_code)] // held to keep the toplevel role alive
    toplevel: XdgToplevel,
    // Single-owner protocol objects for window-control commands, owned by this
    // thread. `seat` also drives interactive move/resize grabs.
    seat: Option<WlSeat>,
    #[cfg(feature = "kde-palette")]
    palette: Option<OrgKdeKwinServerDecorationPalette>,
    shm: WlShm,
    viewport: Option<WpViewport>,
    bg_buffer: Option<crate::wl_state::OwnedBuffer>,
    bg: [u8; 3],
    // Held alive so the compositor keeps delivering preferred_scale.
    #[allow(dead_code)]
    frac_mgr: Option<WpFractionalScaleManagerV1>,
    #[allow(dead_code)]
    frac_scale: Option<WpFractionalScaleV1>,
    #[allow(dead_code)]
    decoration: Option<ZxdgToplevelDecorationV1>,

    current_size: Option<crate::window_state::WindowSize>,
    pending_w: Option<NonZeroI32>,
    pending_h: Option<NonZeroI32>,
    mode: crate::window_state::WindowMode,
    suspended: bool,
    floating: FloatingRestore,
    pending_ack: Option<ConfigureSerial>,
    /// `Some` once the first configure has been acked (the window is "mapped").
    /// Holds the capability that gates buffer attach/commit.
    present: Option<Presented>,
    scale_discovery: ScaleDiscovery,
    pre_fs_maximized: bool,
}

/// Upper bound on the fallback probe: it round-trips on a second display
/// connection, which a wedged compositor can stall forever.
const SCALE_PROBE_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(1);

/// Set by the fallback probe thread once it has fed a scale; the root loop
/// re-plans presentation when it observes this.
static SCALE_FALLBACK_FED: AtomicBool = AtomicBool::new(false);

mod floating_restore {
    use crate::window_state::{WindowMode, WindowSize};

    #[derive(Clone, Copy)]
    pub(super) struct FloatingRestore(Option<WindowSize>);

    impl FloatingRestore {
        pub(super) const EMPTY: Self = Self(None);

        pub(super) fn size(self) -> Option<WindowSize> {
            self.0
        }

        pub(super) fn record(&mut self, mode: WindowMode, w: i32, h: i32) {
            if mode.uses_floating_restore() {
                self.0 = WindowSize::new(w, h);
            }
        }
    }
}
use floating_restore::FloatingRestore;

/// Capability proving a configure has been acked. Buffer attach/commit take a
/// [`Presented`], and the only way to mint one is [`ack`] — so the protocol rule
/// "never commit a buffer before acking a configure" is enforced by the type
/// system, not by comments and a `mapped` bool.
mod present_cap {
    use super::XdgSurface;

    /// A configure serial awaiting ack, consumed by [`ack`].
    #[derive(Clone, Copy)]
    pub(super) struct ConfigureSerial(pub(super) u32);

    /// Zero-sized proof of an acked configure. Its field is private, so it can
    /// only be obtained from [`ack`].
    #[derive(Clone, Copy)]
    pub(super) struct Presented(());

    pub(super) fn ack(xdg: &XdgSurface, serial: ConfigureSerial) -> Presented {
        xdg.ack_configure(serial.0);
        Presented(())
    }
}
use present_cap::{ConfigureSerial, Presented};

/// Pure presentation state machine. Given what the root window currently
/// knows — mapped or not, pending configure or not, scale known or not, and
/// the resolvable logical size — [`presentation::plan`] decides the next step.
/// All Wayland I/O and cross-subsystem notifications stay in the effect layer
/// ([`RootState::try_present`] / [`RootState::execute_present`]).
mod presentation {
    use std::num::NonZeroI32;

    use crate::window_state::{WindowMode, WindowSize};

    /// Everything `plan` needs, free of protocol objects so it is unit-testable.
    #[derive(Clone, Copy)]
    pub(super) struct Inputs {
        /// A configure has been acked before (the window is mapped).
        pub(super) mapped: bool,
        /// An unacked configure is pending.
        pub(super) pending_ack: bool,
        pub(super) scale_known: bool,
        pub(super) size: Option<WindowSize>,
    }

    /// Progress of the deferred first-configure scale fallback.
    ///
    /// Ordering, per compositor style:
    /// - Normal compositors send `preferred_scale` before or alongside the
    ///   first configure. The scale is known when the configure dispatches,
    ///   `plan` never returns `DiscoverScale`, and this stays `Idle`.
    /// - Hyprland-style compositors withhold `preferred_scale` until the
    ///   window maps — which never happens while the first buffer waits on the
    ///   scale. The configure handler then requests discovery (`Requested`),
    ///   but the probe is NOT run inside the callback: the root loop first
    ///   finishes dispatching the current event batch, so a `preferred_scale`
    ///   queued later in the same batch wins and dissolves the request. Only
    ///   if the scale is still unknown after the drain does the loop spawn the
    ///   bounded off-thread probe (`Spawned`), which feeds a provisional scale
    ///   (or the unit fallback) back through `window_state` and wakes the root
    ///   thread to present. The authoritative `preferred_scale` corrects it
    ///   after map.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub(super) enum ScaleDiscovery {
        Idle,
        Requested,
        Spawned,
    }

    impl ScaleDiscovery {
        /// `plan` chose [`Step::DiscoverScale`] inside an event callback: only
        /// note the request; never probe here.
        pub(super) fn request(self) -> Self {
            match self {
                Self::Idle => Self::Requested,
                other => other,
            }
        }

        /// The event batch is drained: decide whether the probe must actually
        /// run. A scale that arrived meanwhile dissolves the request; a spawned
        /// probe is never re-spawned.
        pub(super) fn after_batch_drained(self, scale_known: bool) -> (Self, bool) {
            match self {
                Self::Requested if scale_known => (Self::Idle, false),
                Self::Requested => (Self::Spawned, true),
                other => (other, false),
            }
        }
    }

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub(super) enum Step {
        /// Nothing presentable: no configure yet, or no resolvable size.
        Wait,
        /// A first configure is waiting but no scale is known; scale discovery
        /// must run, then planning re-runs.
        DiscoverScale,
        /// Ack (if a configure is pending), update geometry, and request the
        /// root commit.
        Present,
    }

    pub(super) fn plan(i: Inputs) -> Step {
        // Never commit a buffer before acking a configure (protocol violation);
        // before the first map that means waiting for one.
        if !i.pending_ack && !i.mapped {
            return Step::Wait;
        }
        if !i.scale_known {
            // Scale can only be missing before the first map; after map the
            // compositor has spoken (or the fallback already fed one).
            return if i.mapped {
                Step::Wait
            } else {
                Step::DiscoverScale
            };
        }
        if i.size.is_none() {
            return Step::Wait;
        }
        Step::Present
    }

    pub(super) fn resolve_logical_size(
        pending: (Option<NonZeroI32>, Option<NonZeroI32>),
        cur: Option<WindowSize>,
        floating: Option<WindowSize>,
        mode: WindowMode,
    ) -> Option<WindowSize> {
        let pick =
            |pending: Option<NonZeroI32>, cur: Option<i32>, floating: Option<i32>| -> Option<i32> {
                if let Some(p) = pending {
                    Some(p.get())
                } else if mode.uses_floating_restore() {
                    floating
                } else {
                    cur
                }
            };
        let w = pick(pending.0, cur.map(|s| s.w()), floating.map(|s| s.w()))?;
        let h = pick(pending.1, cur.map(|s| s.h()), floating.map(|s| s.h()))?;
        WindowSize::new(w, h)
    }
}
use presentation::{ScaleDiscovery, resolve_logical_size};

impl RootState {
    fn resolve_logical(&self) -> Option<crate::window_state::WindowSize> {
        resolve_logical_size(
            (self.pending_w, self.pending_h),
            self.current_size,
            self.floating.size(),
            self.mode,
        )
    }

    /// Effect layer around the pure [`presentation::plan`]: gathers inputs and
    /// runs the decided step's Wayland I/O and notifications. May run inside an
    /// event callback, so it must never block — scale discovery is only
    /// requested here and serviced by the root loop between dispatch batches.
    fn try_present(&mut self) {
        let step = presentation::plan(presentation::Inputs {
            mapped: self.present.is_some(),
            pending_ack: self.pending_ack.is_some(),
            scale_known: crate::window_state::known_scale().is_some(),
            size: self.resolve_logical(),
        });
        match step {
            presentation::Step::Wait => {}
            presentation::Step::DiscoverScale => {
                self.scale_discovery = self.scale_discovery.request();
            }
            presentation::Step::Present => self.execute_present(),
        }
    }

    /// Runs on the root loop after `dispatch_pending` has drained the current
    /// event batch. If a `preferred_scale` arrived later in that batch the
    /// request dissolves; otherwise spawn the bounded off-thread probe. See
    /// [`ScaleDiscovery`] for the full ordering contract.
    fn service_scale_discovery(&mut self) {
        let (next, spawn) = self
            .scale_discovery
            .after_batch_drained(crate::window_state::known_scale().is_some());
        self.scale_discovery = next;
        if !spawn {
            return;
        }
        let spawned = thread::Builder::new()
            .name("wl-scale-fallback".into())
            .spawn(|| {
                match crate::scale_probe::probe_scale_bounded(
                    crate::scale_probe::ProbeTarget::FirstOutput,
                    SCALE_PROBE_TIMEOUT,
                ) {
                    Ok(scale) => {
                        tracing::info!(
                            target: "Main",
                            "root window: no preferred_scale before first configure; using probed scale {scale}"
                        );
                        crate::window_state::feed_scale(
                            scale,
                            crate::window_state::ScaleProvenance::Provisional,
                        );
                    }
                    Err(e) => {
                        tracing::warn!(
                            target: "Main",
                            "root window: no preferred_scale before first configure and probe failed ({e}); assuming scale 1.0"
                        );
                        crate::window_state::feed_unit_scale();
                    }
                }
                SCALE_FALLBACK_FED.store(true, Ordering::Release);
                wake_root_thread();
            })
            .is_ok();
        if !spawned {
            // No thread, no probe: unblock presentation with the unit fallback.
            crate::window_state::feed_unit_scale();
            self.try_present();
        }
    }

    fn execute_present(&mut self) {
        let Some(size) = self.resolve_logical() else {
            return;
        };
        let (w, h) = (size.w(), size.h());

        let first = self.present.is_none();
        // Acking a pending configure is the only way to mint the Presented that
        // the buffer ops below require, so the ack necessarily precedes them. On
        // a size/scale-driven re-present (no new configure) we reuse the token
        // from the first ack.
        let present = if let Some(serial) = self.pending_ack.take() {
            let p = present_cap::ack(&self.xdg_surface, serial);
            self.present = Some(p);
            p
        } else if let Some(p) = self.present {
            p
        } else {
            return;
        };
        // Never commit the root here: the loop's latch drain issues the one root
        // commit that presents geometry with the overlay/video subtree.
        self.xdg_surface.set_window_geometry(0, 0, w, h);
        self.fill_background(w, h, present);
        self.current_size = Some(size);
        self.floating.record(self.mode, w, h);
        if first {
            tracing::info!(target: "Main", "root window: first configure {w}x{h} (app toplevel is live)");
        }

        // Pass logical (not physical) size: mpv and the overlay apply scale
        // themselves, so a physical size here would double-scale.
        crate::mpv_proxy::set_window_size(size);
        crate::window_state::publish(size, self.mode);

        PENDING_PRESENT.store(true, Ordering::Release);
    }

    fn present_transaction(&mut self, _present: Presented) {
        self.surface.commit();
    }

    fn fill_background(&mut self, w: i32, h: i32, _present: Presented) {
        if let Some(vp) = &self.viewport {
            vp.set_destination(w, h);
        }
        if self.bg_buffer.is_none() {
            self.bg_buffer = self.create_solid_buffer();
            if let Some(buf) = &self.bg_buffer {
                buf.attach_to(&self.surface, 0, 0);
            }
        }
        crate::wl_state::damage_all(&self.surface);
    }

    fn rebuild_background(&mut self, w: i32, h: i32, _present: Presented) {
        // Build the replacement before retiring the current buffer so an
        // allocation failure leaves a valid buffer owned rather than none.
        let Some(new) = self.create_solid_buffer() else {
            return;
        };
        new.attach_to(&self.surface, 0, 0);
        if let Some(old) = self.bg_buffer.replace(new) {
            crate::wl_state::retire_buffer(old);
        }
        if let Some(vp) = &self.viewport {
            vp.set_destination(w, h);
        }
        crate::wl_state::damage_all(&self.surface);
    }

    fn create_solid_buffer(&self) -> Option<crate::wl_state::OwnedBuffer> {
        let bg = self.bg;
        crate::wl_state::build_argb8888_shm_buffer(
            &self.shm,
            &self.qh,
            "root-bg",
            1,
            1,
            move |dst| {
                // ARGB8888 little-endian byte order = [B, G, R, A].
                dst.copy_from_slice(&[bg[2], bg[1], bg[0], 0xFF]);
                true
            },
        )
    }
}

static STARTED: AtomicBool = AtomicBool::new(false);

/// Opaque handle to the app root `wl_surface`, carrying the live `wl_proxy`
/// pointer — the only representation valid across the two wayland-client
/// `Backend`s that share this one `wl_display` — so `wl_state` can rebuild the
/// surface under its own `Backend` via `ObjectId::from_ptr`.
#[derive(Copy, Clone)]
pub(crate) struct RootSurfaceHandle(std::ptr::NonNull<c_void>);

// Process-lifetime `wl_proxy` owned by the root thread; the handle only
// republishes it for reconstruction and never destroys it.
unsafe impl Send for RootSurfaceHandle {}
unsafe impl Sync for RootSurfaceHandle {}

impl RootSurfaceHandle {
    pub(crate) fn as_ptr(self) -> *mut c_void {
        self.0.as_ptr()
    }
}

static ROOT_SURFACE: OnceLock<RootSurfaceHandle> = OnceLock::new();

pub(crate) fn root_surface_handle() -> Option<RootSurfaceHandle> {
    ROOT_SURFACE.get().copied()
}

// Window-control requests queued here and applied on the root thread by
// `apply_command`. The toplevel/seat proxies are single-owner and live on that
// thread, so requests cross this queue rather than caching proxy clones that
// could be used after teardown. Move/resize carry the input serial captured at
// request time.
enum WindowCommand {
    Move {
        serial: u32,
    },
    Resize {
        serial: u32,
        edge: u32,
    },
    SetMaximized(bool),
    Minimize,
    #[cfg(feature = "kde-palette")]
    SetTitlebarPalette(String),
}

static COMMANDS: Mutex<Vec<WindowCommand>> = Mutex::new(Vec::new());

fn push_command(cmd: WindowCommand) {
    COMMANDS.lock().push(cmd);
    wake_root_thread();
}

fn apply_command(state: &mut RootState, cmd: WindowCommand) {
    match cmd {
        WindowCommand::Move { serial } => {
            if let Some(seat) = &state.seat {
                state.toplevel._move(seat, serial);
            } else {
                // Not re-queued: the serial is only valid for the input event it
                // came from, so replaying it once a seat exists would be stale.
                tracing::warn!(target: "Main", "interactive move dropped: no seat");
            }
        }
        WindowCommand::Resize { serial, edge } => {
            if let Some(seat) = &state.seat {
                match xdg_toplevel::ResizeEdge::try_from(edge) {
                    Ok(e) => state.toplevel.resize(seat, serial, e),
                    Err(_) => {
                        tracing::warn!(target: "Main", "interactive resize dropped: bad edge {edge}");
                    }
                }
            } else {
                tracing::warn!(target: "Main", "interactive resize dropped: no seat");
            }
        }
        WindowCommand::SetMaximized(on) => {
            if on {
                state.toplevel.set_maximized();
            } else {
                state.toplevel.unset_maximized();
            }
        }
        WindowCommand::Minimize => state.toplevel.set_minimized(),
        #[cfg(feature = "kde-palette")]
        WindowCommand::SetTitlebarPalette(path) => {
            if let Some(p) = &state.palette {
                p.set_palette(path);
            } else {
                tracing::warn!(target: "Main", "titlebar palette dropped: no palette manager");
            }
        }
    }
    let _ = state.conn.flush();
}

pub(crate) fn start_move() {
    push_command(WindowCommand::Move {
        serial: crate::input::last_button_serial(),
    });
}

pub(crate) fn start_resize(edge: u32) {
    push_command(WindowCommand::Resize {
        serial: crate::input::last_button_serial(),
        edge,
    });
}

// Fullscreen requests posted here and applied on the root thread by
// `apply_fullscreen`. The mode read and the protocol request must stay on that
// thread — the sole mutator/reader of `RootState.mode` — so a configure can't
// flip the mode between them and make toggle send the wrong command.
const FS_NONE: u8 = 0;
const FS_TOGGLE: u8 = 1;
const FS_ON: u8 = 2;
const FS_OFF: u8 = 3;
static PENDING_FS: AtomicU8 = AtomicU8::new(FS_NONE);

pub(crate) fn set_fullscreen(on: bool) {
    PENDING_FS.store(if on { FS_ON } else { FS_OFF }, Ordering::Release);
    wake_root_thread();
}

pub(crate) fn toggle_fullscreen() {
    PENDING_FS.store(FS_TOGGLE, Ordering::Release);
    wake_root_thread();
}

fn apply_fullscreen(state: &mut RootState, on: bool) {
    if on {
        // A fullscreen-enter received while already fullscreen must not overwrite
        // the saved restore mode, so capture it only when entering from another mode.
        if !matches!(state.mode, crate::window_state::WindowMode::Fullscreen) {
            state.pre_fs_maximized =
                matches!(state.mode, crate::window_state::WindowMode::Maximized);
        }
        state.toplevel.set_fullscreen(None);
    } else {
        state.toplevel.unset_fullscreen();
        // The compositor need not restore the pre-fullscreen maximized state, so
        // re-request it (the final mode is still confirmed via a configure).
        if state.pre_fs_maximized {
            state.toplevel.set_maximized();
            state.pre_fs_maximized = false;
        }
    }
    let _ = state.conn.flush();
}

pub(crate) fn set_maximized(on: bool) {
    push_command(WindowCommand::SetMaximized(on));
}

// Commanded maximize state for the CSD toggle button. Mirrored from the
// compositor on every configure so a compositor-initiated maximize doesn't
// desync the toggle.
static MAXIMIZED: AtomicBool = AtomicBool::new(false);

pub(crate) fn toggle_maximize() {
    let next = !MAXIMIZED.load(Ordering::Relaxed);
    MAXIMIZED.store(next, Ordering::Relaxed);
    set_maximized(next);
}

pub(crate) fn sync_maximized_command_state(maximized: bool) {
    MAXIMIZED.store(maximized, Ordering::Relaxed);
}

pub(crate) fn set_minimized() {
    push_command(WindowCommand::Minimize);
}

pub(crate) struct PopupShell {
    conn: Connection,
    qh: QueueHandle<RootState>,
    compositor: WlCompositor,
    viewporter: Option<WpViewporter>,
    shm: WlShm,
    wm_base: XdgWmBase,
    root_xdg: XdgSurface,
    seat: Option<WlSeat>,
}

static POPUP_SHELL: OnceLock<PopupShell> = OnceLock::new();

pub(crate) fn popup_shell() -> Option<&'static PopupShell> {
    POPUP_SHELL.get()
}

impl PopupShell {
    pub(crate) fn create_surface(&self) -> WlSurface {
        self.compositor.create_surface(&self.qh, ())
    }

    pub(crate) fn create_viewport(&self, surface: &WlSurface) -> Option<WpViewport> {
        self.viewporter
            .as_ref()
            .map(|v| v.get_viewport(surface, &self.qh, ()))
    }

    pub(crate) fn create_shm_buffer(
        &self,
        pixels: &[u8],
        w: i32,
        h: i32,
    ) -> Option<crate::wl_state::OwnedBuffer> {
        crate::wl_state::build_shm_buffer_from_pixels(&self.shm, &self.qh, "menu-sw", pixels, w, h)
    }

    pub(crate) fn flush(&self) {
        let _ = self.conn.flush();
    }
}

// Ties a configure/popup_done back to the menu generation that owns it, so a
// late event from a torn-down popup is ignored.
#[derive(Clone, Copy)]
struct PopupRole {
    generation: NonZeroU64,
}

struct PopupRoleObjs {
    xdg: Option<XdgSurface>,
    popup: Option<XdgPopup>,
    generation: Option<NonZeroU64>,
}
static POPUP_ROLE: Mutex<PopupRoleObjs> = Mutex::new(PopupRoleObjs {
    xdg: None,
    popup: None,
    generation: None,
});

// Highest menu generation ever armed; generations come from a monotonic counter.
// create runs on the input/CEF thread while destroy/reposition run on the root
// thread, so a call delayed past a newer arm carries a stale generation. Rejecting
// against this stops a stale create from tearing down the newer popup and a stale
// reposition from retargeting it.
static ARMED_GEN: AtomicU64 = AtomicU64::new(0);

fn build_menu_positioner(shell: &PopupShell, x: i32, y: i32, w: i32, h: i32) -> XdgPositioner {
    let p = shell.wm_base.create_positioner(&shell.qh, ());
    p.set_size(w.max(1), h.max(1));
    p.set_anchor_rect(x, y, 1, 1);
    p.set_anchor(Anchor::TopLeft);
    p.set_gravity(Gravity::BottomRight);
    p.set_constraint_adjustment(
        ConstraintAdjustment::FlipX
            | ConstraintAdjustment::FlipY
            | ConstraintAdjustment::SlideX
            | ConstraintAdjustment::SlideY,
    );
    p
}

/// Create the grab popup for `surface`. The grab cites the input thread's last
/// button serial — valid here only because every app connection shares one
/// wl_client.
pub(crate) fn popup_create(
    generation: NonZeroU64,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    surface: &WlSurface,
) {
    let Some(shell) = popup_shell() else {
        return;
    };
    // Hold POPUP_ROLE across teardown of the old role and publication of the new
    // one: without this a concurrent popup_destroy/popup_reposition could run in
    // the gap, observe an empty slot, and leave the just-created popup live but
    // unpublished — a torn create/use span.
    let mut role = POPUP_ROLE.lock();
    // Each generation drives exactly one create, so `<=` (not `<`) also blocks
    // resurrecting a just-destroyed popup, since destroy leaves ARMED_GEN at its peak.
    if generation.get() <= ARMED_GEN.load(Ordering::Acquire) {
        return;
    }
    ARMED_GEN.store(generation.get(), Ordering::Release);
    destroy_role_objs(&mut role);
    let positioner = build_menu_positioner(shell, x, y, w, h);
    let xdg = shell
        .wm_base
        .get_xdg_surface(surface, &shell.qh, PopupRole { generation });
    let popup = xdg.get_popup(
        Some(&shell.root_xdg),
        &positioner,
        &shell.qh,
        PopupRole { generation },
    );
    positioner.destroy();
    if let Some(seat) = &shell.seat {
        popup.grab(seat, crate::input::last_button_serial());
    }
    surface.commit();
    shell.flush();
    role.xdg = Some(xdg);
    role.popup = Some(popup);
    role.generation = Some(generation);
}

/// Requires the popup to already be mapped.
pub(crate) fn popup_reposition(generation: NonZeroU64, x: i32, y: i32, w: i32, h: i32) {
    let Some(shell) = popup_shell() else {
        return;
    };
    let positioner = build_menu_positioner(shell, x, y, w, h);
    {
        // Reposition must be issued under POPUP_ROLE: popup_destroy runs on the
        // root thread and will otherwise destroy the popup mid-request, leaving
        // this a request on a dead object that drops the client.
        let role = POPUP_ROLE.lock();
        if role.generation == Some(generation)
            && let Some(popup) = role.popup.as_ref()
        {
            popup.reposition(&positioner, 0);
            shell.flush();
        }
    }
    positioner.destroy();
}

/// Destroys only the popup role objects, not the menu wl_surface — that surface
/// is persistent (owned by crate::popup) and re-roled on the next open.
fn destroy_role_objs(role: &mut PopupRoleObjs) {
    if let Some(p) = role.popup.take() {
        p.destroy();
    }
    if let Some(x) = role.xdg.take() {
        x.destroy();
    }
    role.generation = None;
}

/// Tear down the popup role, but only if `generation` still owns it — a newer
/// `arm` may have published a fresh role in the gap between a stale teardown
/// deciding to destroy and this call, and must not be torn down by it. Unqualified
/// force-destroy stays private (`destroy_role_objs`), reached only from
/// `popup_create` under the `ARMED_GEN` guard.
pub(crate) fn popup_destroy(generation: NonZeroU64) {
    {
        let mut role = POPUP_ROLE.lock();
        if role.generation != Some(generation) {
            return;
        }
        destroy_role_objs(&mut role);
    }
    if let Some(shell) = popup_shell() {
        shell.flush();
    }
}

// High bit marks "set"; the low 24 bits are RGB. Applied on the dispatch thread,
// which owns the surface, so commits don't race the configure handler.
static PENDING_BG: AtomicU32 = AtomicU32::new(0);
const BG_SET: u32 = 1 << 24;

fn wake_root_thread() {
    if let Some(t) = ROOT_THREAD.get() {
        t.wake.signal();
    }
}

pub(crate) fn set_background_color(r: u8, g: u8, b: u8) {
    let rgb = (u32::from(r) << 16) | (u32::from(g) << 8) | u32::from(b);
    PENDING_BG.store(BG_SET | rgb, Ordering::Release);
    wake_root_thread();
}

fn pending_bg() -> Option<[u8; 3]> {
    let v = PENDING_BG.load(Ordering::Acquire);
    (v & BG_SET != 0).then_some([(v >> 16) as u8, (v >> 8) as u8, v as u8])
}

// The root `wl_surface.commit` is issued by exactly one owner — this dispatch
// thread. Every other producer (CEF paint paths, mpv) that needs to present
// requests it here, so geometry, overlay and video always land in one
// uninterruptible root commit; no other thread can commit the root between a
// geometry change and its children.
static PENDING_PRESENT: AtomicBool = AtomicBool::new(false);

pub(crate) fn request_present() {
    PENDING_PRESENT.store(true, Ordering::Release);
    wake_root_thread();
}

#[cfg(feature = "kde-palette")]
pub(crate) fn set_titlebar_palette(path: &std::path::Path) {
    if let Some(s) = path.to_str() {
        push_command(WindowCommand::SetTitlebarPalette(s.to_owned()));
    }
}

// Teardown handle for the dispatch thread. Without it the thread sits in
// `poll(-1)` holding a `wl_display` read barrier; when no video ever played the
// display is quiet, so the barrier is never released and mpv's VO-teardown
// roundtrip hangs forever. `cleanup` signals + joins before that roundtrip.
struct RootThread {
    stop: Arc<AtomicBool>,
    wake: Arc<jfn_wake_event::WakeEvent>,
    handle: Mutex<Option<JoinHandle<()>>>,
}
static ROOT_THREAD: OnceLock<RootThread> = OnceLock::new();

/// Stop and join the dispatch thread, releasing its `wl_display` read barrier.
/// Must run before mpv's VO teardown, or that roundtrip deadlocks on the barrier.
pub(crate) fn cleanup() {
    let Some(t) = ROOT_THREAD.get() else {
        return;
    };
    t.stop.store(true, Ordering::Relaxed);
    wake_root_thread();
    if let Some(h) = t.handle.lock().take() {
        let _ = h.join();
        // The WakeEvent's fd is owned by this process-lifetime RootThread and
        // closes with it; no manual close.
    }
}

fn vo_display() -> Option<crate::app_conn::AppDisplay> {
    crate::app_conn::app_display()
}

/// Create the app-owned toplevel and start its dispatch thread. The toplevel
/// must exist before the VO-wait gate (which reads its size + scale), but the
/// mpv VO display it needs only appears mid-wait — so this is idempotent and
/// polled each tick until the display is available.
pub(crate) fn ensure_started() {
    if STARTED.load(Ordering::Acquire) {
        return;
    }
    let Some(display) = vo_display() else {
        return;
    };
    if STARTED.swap(true, Ordering::AcqRel) {
        return;
    }

    let backend =
        unsafe { wayland_backend::client::Backend::from_foreign_display(display.as_ptr().cast()) };
    let conn = Connection::from_backend(backend);
    let (globals, queue) = match registry_queue_init::<RootState>(&conn) {
        Ok(g) => g,
        Err(e) => {
            tracing::error!(target: "Main", "root window: registry init: {e}");
            return;
        }
    };
    let qh = queue.handle();

    let compositor: WlCompositor = match globals.bind(&qh, 1..=4, ()) {
        Ok(c) => c,
        Err(e) => {
            tracing::error!(target: "Main", "root window: bind wl_compositor: {e}");
            return;
        }
    };
    let shm: WlShm = match globals.bind(&qh, 1..=1, ()) {
        Ok(s) => s,
        Err(e) => {
            tracing::error!(target: "Main", "root window: bind wl_shm: {e}");
            return;
        }
    };
    let viewporter: Option<WpViewporter> = globals.bind(&qh, 1..=1, ()).ok();

    let wm_base: XdgWmBase = match globals.bind(&qh, 1..=6, ()) {
        Ok(b) => b,
        Err(e) => {
            tracing::error!(target: "Main", "root window: bind xdg_wm_base: {e}");
            return;
        }
    };

    let surface = compositor.create_surface(&qh, ());
    // Publish the root wl_proxy so wl_state can parent its CEF overlay under this
    // surface: same libwayland wl_display, but a different wayland-client Backend,
    // so it must be reconstructed there via ObjectId::from_ptr.
    if let Some(p) = std::ptr::NonNull::new(surface.id().as_ptr().cast()) {
        let _ = ROOT_SURFACE.set(RootSurfaceHandle(p));
    }
    let xdg_surface = wm_base.get_xdg_surface(&surface, &qh, ());
    let toplevel = xdg_surface.get_toplevel(&qh, ());
    toplevel.set_title(TITLE.to_owned());
    toplevel.set_app_id(APP_ID.to_owned());

    let (boot_w, boot_h, boot_max) = boot_geometry();
    if boot_max {
        toplevel.set_maximized();
    }

    let viewport = viewporter
        .as_ref()
        .map(|vp| vp.get_viewport(&surface, &qh, ()));
    if viewport.is_none() {
        tracing::warn!(target: "Main", "root window: no wp_viewporter; background unscaled");
    }

    let frac_mgr: Option<WpFractionalScaleManagerV1> = globals.bind(&qh, 1..=1, ()).ok();
    let frac_scale = frac_mgr
        .as_ref()
        .map(|m| m.get_fractional_scale(&surface, &qh, ()));
    if frac_mgr.is_none() {
        // No preferred_scale will ever arrive, so satisfy the boot scale gate —
        // otherwise it waits forever.
        tracing::warn!(target: "Main", "root window: no wp_fractional_scale_manager_v1; assuming scale 1.0");
        crate::window_state::feed_unit_scale();
    }

    // Request server/client-side decorations to match the configured mode.
    // Without an explicit request a compositor's default (KWin: server-side,
    // sway: none) leaves the window with no titlebar.
    let deco_mgr: Option<ZxdgDecorationManagerV1> = globals.bind(&qh, 1..=1, ()).ok();
    let decoration = deco_mgr.as_ref().map(|mgr| {
        let dec = mgr.get_toplevel_decoration(&toplevel, &qh, ());
        let mode = if WANT_CSD.load(Ordering::Acquire) {
            DecorationMode::ClientSide
        } else {
            DecorationMode::ServerSide
        };
        dec.set_mode(mode);
        dec
    });
    if deco_mgr.is_none() {
        tracing::warn!(target: "Main", "root window: no zxdg_decoration_manager_v1");
    }

    #[cfg(feature = "kde-palette")]
    let palette: Option<OrgKdeKwinServerDecorationPalette> = globals
        .bind::<OrgKdeKwinServerDecorationPaletteManager, _, _>(&qh, 1..=1, ())
        .ok()
        .map(|mgr| mgr.create(&surface, &qh, ()));

    let seat: Option<WlSeat> = globals.bind(&qh, 1..=8, ()).ok();

    let _ = POPUP_SHELL.set(PopupShell {
        conn: conn.clone(),
        qh: qh.clone(),
        compositor: compositor.clone(),
        viewporter: viewporter.clone(),
        shm: shm.clone(),
        wm_base: wm_base.clone(),
        root_xdg: xdg_surface.clone(),
        seat: seat.clone(),
    });

    xdg_surface.set_window_geometry(0, 0, boot_w, boot_h);
    // Roleless commit (no buffer attached) to elicit the first
    // xdg_surface.configure — and, on compositors that send preferred_scale only
    // in response to a commit, the first scale. It must not be gated on scale:
    // xdg-shell requires this commit to obtain the configure that scale may
    // itself depend on.
    surface.commit();
    let _ = conn.flush();

    let state = RootState {
        conn: conn.clone(),
        qh,
        surface,
        xdg_surface,
        toplevel,
        seat,
        #[cfg(feature = "kde-palette")]
        palette,
        shm: shm.clone(),
        viewport,
        bg_buffer: None,
        bg: pending_bg().unwrap_or(BG),
        frac_mgr,
        frac_scale,
        decoration,
        current_size: None,
        pending_w: None,
        pending_h: None,
        mode: crate::window_state::WindowMode::Floating,
        suspended: false,
        floating: {
            let mut f = FloatingRestore::EMPTY;
            f.record(crate::window_state::WindowMode::Floating, boot_w, boot_h);
            f
        },
        pending_ack: None,
        present: None,
        scale_discovery: ScaleDiscovery::Idle,
        pre_fs_maximized: false,
    };

    let Some(wake) = jfn_wake_event::WakeEvent::new().map(Arc::new) else {
        tracing::error!(target: "Main", "root window: eventfd failed");
        return;
    };
    let stop = Arc::new(AtomicBool::new(false));
    let stop_thread = stop.clone();
    let wake_thread = wake.clone();
    match thread::Builder::new()
        .name("wl-root".into())
        .spawn(move || root_loop(queue, state, wake_thread, stop_thread))
    {
        Ok(handle) => {
            let _ = ROOT_THREAD.set(RootThread {
                stop,
                wake,
                handle: Mutex::new(Some(handle)),
            });
        }
        Err(e) => {
            tracing::error!(target: "Main", "root window: thread spawn: {e}");
        }
    }
}

// Apply queued fullscreen / window-control / background-color requests. Runs on
// the root thread each iteration before it blocks, so a request enqueued before
// the wake fd could ring is still serviced without waiting for another event.
fn service_root_requests(state: &mut RootState) {
    match PENDING_FS.swap(FS_NONE, Ordering::Acquire) {
        FS_ON => apply_fullscreen(state, true),
        FS_OFF => apply_fullscreen(state, false),
        FS_TOGGLE => {
            let on = !matches!(state.mode, crate::window_state::WindowMode::Fullscreen);
            apply_fullscreen(state, on);
        }
        _ => {}
    }
    // Drain into a local first so the queue lock isn't held while issuing
    // protocol requests.
    let cmds = std::mem::take(&mut *COMMANDS.lock());
    for cmd in cmds {
        apply_command(state, cmd);
    }
    if let Some(bg) = pending_bg()
        && bg != state.bg
    {
        state.bg = bg;
        // current_size is only set once presented, so the capability is present
        // too; requiring it keeps the buffer attach behind an ack.
        if let (Some(size), Some(present)) = (state.current_size, state.present) {
            let (w, h) = (size.w(), size.h());
            state.rebuild_background(w, h, present);
            // Apply via the single owner commit, not a standalone one.
            PENDING_PRESENT.store(true, Ordering::Release);
        }
    }
}

// Coordinates with the other readers on the shared fd via prepare_read + poll
// (a blocking dispatch here would deadlock them). A wake eventfd lets `cleanup`
// break the poll so the read barrier is released at shutdown.
fn root_loop(
    mut queue: EventQueue<RootState>,
    mut state: RootState,
    wake: Arc<jfn_wake_event::WakeEvent>,
    stop: Arc<AtomicBool>,
) {
    let conn = state.conn.clone();
    let fd = conn.as_fd().as_raw_fd();
    let wake_fd = wake.fd();
    loop {
        if queue.dispatch_pending(&mut state).is_err() {
            break;
        }
        // The batch is drained: a preferred_scale queued behind the configure
        // has dispatched by now, so only a genuinely absent scale spawns the
        // fallback probe — and a completed probe re-plans presentation here,
        // on the thread that owns the surface.
        if SCALE_FALLBACK_FED.swap(false, Ordering::AcqRel) {
            state.try_present();
        }
        state.service_scale_discovery();
        // Service queued control work before the blocking poll, not only after a
        // wake: wake_root_thread is a no-op until ROOT_THREAD is published, so a
        // request stored during that startup window rings no fd and would
        // otherwise sleep here until an unrelated compositor event arrives.
        service_root_requests(&mut state);
        // Drain here, before the blocking poll: an event handler (configure,
        // scale) that raised the latch during dispatch must commit now, or the
        // loop blocks in poll with the compositor still awaiting our commit.
        // Gate on the present capability so a pre-configure request stays
        // latched, not lost — swapping the latch only once we can present.
        if let Some(present) = state.present
            && PENDING_PRESENT.swap(false, Ordering::Acquire)
        {
            state.present_transaction(present);
        }
        let _ = conn.flush();

        // A probe completion between the check above and here must not strand
        // its result until the next compositor event (its wake can be lost if
        // ROOT_THREAD isn't published yet); re-run the loop instead of polling.
        if SCALE_FALLBACK_FED.load(Ordering::Acquire) {
            continue;
        }
        let guard = match queue.prepare_read() {
            Some(g) => g,
            None => continue,
        };
        let mut pfds = [
            libc::pollfd {
                fd,
                events: libc::POLLIN,
                revents: 0,
            },
            libc::pollfd {
                fd: wake_fd,
                events: libc::POLLIN,
                revents: 0,
            },
        ];
        let r = unsafe { libc::poll(pfds.as_mut_ptr(), pfds.len() as _, -1) };
        if r < 0 {
            let err = std::io::Error::last_os_error();
            drop(guard);
            if err.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            break;
        }
        if pfds[0].revents & libc::POLLIN != 0 {
            if guard.read().is_err() {
                break;
            }
            // This thread is the sole reader of the shared display; the read
            // above distributes events to every queue on it. Pump the CEF
            // overlay queue so its `wl_buffer.release` events are processed and
            // retired buffers get destroyed.
            crate::wl_state::pump_events();
        } else {
            drop(guard);
        }
        if pfds[0].revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
            break;
        }
        if pfds[1].revents & libc::POLLIN != 0 {
            wake.drain();
            if stop.load(Ordering::Relaxed) {
                break;
            }
        }
    }
    // Do not drain the bg's release here: this thread shares the wl_display fd
    // with the other readers via prepare_read/poll, so a blocking roundtrip
    // would deadlock them.
    if let Some(bg) = state.bg_buffer.take() {
        crate::wl_state::retire_buffer(bg);
    }
}

impl Dispatch<XdgWmBase, ()> for RootState {
    fn event(
        _: &mut Self,
        wm_base: &XdgWmBase,
        event: xdg_wm_base::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_wm_base::Event::Ping { serial } = event {
            wm_base.pong(serial);
        }
    }
}

impl Dispatch<XdgSurface, ()> for RootState {
    fn event(
        state: &mut Self,
        _: &XdgSurface,
        event: xdg_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_surface::Event::Configure { serial } = event {
            // Coalesce to the latest serial; the toplevel.configure that carries
            // the size/states precedes this in wire order, so pending_w/h + mode
            // are already current.
            state.pending_ack = Some(ConfigureSerial(serial));
            state.try_present();
        }
    }
}

impl Dispatch<XdgToplevel, ()> for RootState {
    fn event(
        state: &mut Self,
        _: &XdgToplevel,
        event: xdg_toplevel::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            xdg_toplevel::Event::Configure {
                width,
                height,
                states,
            } => {
                state.pending_w = NonZeroI32::new(width);
                state.pending_h = NonZeroI32::new(height);
                let (mut fs, mut max, mut tiled, mut suspended) = (false, false, false, false);
                for chunk in states.chunks_exact(4) {
                    match u32::from_ne_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]) {
                        STATE_FULLSCREEN => fs = true,
                        STATE_MAXIMIZED => max = true,
                        STATE_TILED_LEFT | STATE_TILED_RIGHT | STATE_TILED_TOP
                        | STATE_TILED_BOTTOM => tiled = true,
                        STATE_SUSPENDED => suspended = true,
                        _ => {}
                    }
                }
                state.mode = if fs {
                    crate::window_state::WindowMode::Fullscreen
                } else if max {
                    crate::window_state::WindowMode::Maximized
                } else if tiled {
                    crate::window_state::WindowMode::Tiled
                } else {
                    crate::window_state::WindowMode::Floating
                };
                if suspended != state.suspended {
                    state.suspended = suspended;
                    crate::window_state::feed_suspended(suspended);
                }
            }
            xdg_toplevel::Event::Close => {
                jfn_playback::shutdown::jfn_shutdown_initiate();
            }
            _ => {}
        }
    }
}

impl Dispatch<WpFractionalScaleV1, ()> for RootState {
    fn event(
        state: &mut Self,
        _: &WpFractionalScaleV1,
        event: wp_fractional_scale_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wp_fractional_scale_v1::Event::PreferredScale { scale } = event {
            let Some(scale) = crate::scale::Scale120::from_wire(scale) else {
                return;
            };
            crate::window_state::feed_scale(
                scale,
                crate::window_state::ScaleProvenance::Authoritative,
            );
            // Scale arrives without a configure (output change, or the first
            // scale completing a withheld configure), so drive a present here too.
            state.try_present();
        }
    }
}

// Distinct PopupRole userdata keeps this off the root toplevel's `()`-keyed
// XdgSurface dispatch; sharing `()` would route popup configures into the root's
// configure handler.
impl Dispatch<XdgSurface, PopupRole> for RootState {
    fn event(
        _: &mut Self,
        xdg: &XdgSurface,
        event: xdg_surface::Event,
        role: &PopupRole,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_surface::Event::Configure { serial } = event {
            xdg.ack_configure(serial);
            crate::popup::on_ready(role.generation);
        }
    }
}

impl Dispatch<XdgPopup, PopupRole> for RootState {
    fn event(
        _: &mut Self,
        _: &XdgPopup,
        event: xdg_popup::Event,
        role: &PopupRole,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let xdg_popup::Event::PopupDone = event {
            crate::popup::on_done(role.generation);
            popup_destroy(role.generation);
        }
    }
}

macro_rules! noop_dispatch {
    ($($ty:ty),+ $(,)?) => {
        $(impl Dispatch<$ty, ()> for RootState {
            fn event(
                _: &mut Self,
                _: &$ty,
                _: <$ty as Proxy>::Event,
                _: &(),
                _: &Connection,
                _: &QueueHandle<Self>,
            ) {}
        })+
    };
}

noop_dispatch!(
    WlSurface,
    WlCompositor,
    WlShm,
    WlShmPool,
    WpViewporter,
    WpViewport,
    WpFractionalScaleManagerV1,
    ZxdgDecorationManagerV1,
    WlSeat,
    XdgPositioner,
);

impl Dispatch<WlBuffer, ()> for RootState {
    fn event(
        _: &mut Self,
        buffer: &WlBuffer,
        event: <WlBuffer as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wayland_client::protocol::wl_buffer::Event::Release = event {
            crate::wl_state::note_buffer_release(buffer);
        }
    }
}

impl Dispatch<ZxdgToplevelDecorationV1, ()> for RootState {
    fn event(
        _: &mut Self,
        _: &ZxdgToplevelDecorationV1,
        _: zxdg_toplevel_decoration_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

#[cfg(feature = "kde-palette")]
impl Dispatch<OrgKdeKwinServerDecorationPaletteManager, ()> for RootState {
    fn event(
        _: &mut Self,
        _: &OrgKdeKwinServerDecorationPaletteManager,
        _: <OrgKdeKwinServerDecorationPaletteManager as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

#[cfg(feature = "kde-palette")]
impl Dispatch<OrgKdeKwinServerDecorationPalette, ()> for RootState {
    fn event(
        _: &mut Self,
        _: &OrgKdeKwinServerDecorationPalette,
        _: <OrgKdeKwinServerDecorationPalette as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<WlRegistry, GlobalListContents> for RootState {
    fn event(
        _: &mut Self,
        _: &WlRegistry,
        _: <WlRegistry as Proxy>::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

#[cfg(test)]
mod model_tests {
    //! Model-driven suite for the root-window startup/scale state machine.
    //! [`Model`] mirrors the effect layer exactly — every decision routes
    //! through the same pure functions the live code uses (`plan`,
    //! `ScaleDiscovery::{request, after_batch_drained}`, `scale_displaces`) —
    //! while recording effects (acks, commits, probe spawns) for assertion.

    use super::presentation::{Inputs, ScaleDiscovery, Step, plan};
    use crate::window_state::{ScaleProvenance, WindowSize, scale_displaces};

    struct Model {
        mapped: bool,
        pending_ack: bool,
        scale: Option<ScaleProvenance>,
        discovery: ScaleDiscovery,
        acks: u32,
        commits: u32,
        probe_spawns: u32,
    }

    #[derive(Clone, Copy)]
    enum Ev {
        /// xdg_surface.configure dispatches (toplevel size/states precede it).
        Configure,
        /// wp_fractional_scale.preferred_scale dispatches.
        PreferredScale,
        /// dispatch_pending returned: the current batch is fully drained.
        BatchDrained,
        /// The bounded fallback probe fed a provisional scale (probe result or
        /// unit fallback — identical control flow).
        ProbeCompletes,
        /// Boot found no wp_fractional_scale_manager_v1.
        BootUnitScale,
    }

    impl Model {
        fn new() -> Self {
            Self {
                mapped: false,
                pending_ack: false,
                scale: None,
                discovery: ScaleDiscovery::Idle,
                acks: 0,
                commits: 0,
                probe_spawns: 0,
            }
        }

        fn feed(&mut self, incoming: ScaleProvenance) {
            if scale_displaces(self.scale, incoming) {
                self.scale = Some(incoming);
            }
        }

        /// Mirrors `RootState::try_present`.
        fn drive(&mut self) {
            let step = plan(Inputs {
                mapped: self.mapped,
                pending_ack: self.pending_ack,
                scale_known: self.scale.is_some(),
                // The boot floating size is always recorded, so a floating
                // root always resolves; size-resolution corner cases are
                // covered by the resolve_logical_size tests.
                size: WindowSize::new(1280, 720),
            });
            match step {
                Step::Wait => {}
                Step::DiscoverScale => self.discovery = self.discovery.request(),
                Step::Present => {
                    // Protocol invariant: buffer operations only ever follow a
                    // configure ack (fresh here, or the one that mapped us).
                    assert!(
                        self.pending_ack || self.mapped,
                        "present without any acked configure"
                    );
                    if self.pending_ack {
                        self.pending_ack = false;
                        self.acks += 1;
                        self.mapped = true;
                    }
                    // One latch raise = exactly one root commit per transaction.
                    self.commits += 1;
                }
            }
        }

        fn ev(&mut self, e: Ev) {
            match e {
                Ev::Configure => {
                    self.pending_ack = true;
                    self.drive();
                }
                Ev::PreferredScale => {
                    self.feed(ScaleProvenance::Authoritative);
                    self.drive();
                }
                Ev::BatchDrained => {
                    let (next, spawn) = self.discovery.after_batch_drained(self.scale.is_some());
                    self.discovery = next;
                    if spawn {
                        self.probe_spawns += 1;
                    }
                }
                Ev::ProbeCompletes => {
                    self.feed(ScaleProvenance::Provisional);
                    self.drive();
                }
                Ev::BootUnitScale => self.feed(ScaleProvenance::Provisional),
            }
        }

        fn run(&mut self, evs: &[Ev]) {
            for &e in evs {
                self.ev(e);
            }
        }
    }

    #[test]
    fn preferred_scale_before_configure() {
        let mut m = Model::new();
        m.run(&[Ev::PreferredScale]);
        // Scale alone must not touch the surface before the first configure.
        assert_eq!(m.commits, 0);
        m.run(&[Ev::Configure, Ev::BatchDrained]);
        assert_eq!((m.acks, m.commits, m.probe_spawns), (1, 1, 0));
        assert_eq!(m.discovery, ScaleDiscovery::Idle);
    }

    #[test]
    fn preferred_scale_later_in_same_batch_wins_without_probe() {
        let mut m = Model::new();
        m.run(&[Ev::Configure]);
        // Mid-batch: the callback only requested discovery — it must not probe.
        assert_eq!(m.discovery, ScaleDiscovery::Requested);
        assert_eq!(m.commits, 0);
        m.run(&[Ev::PreferredScale, Ev::BatchDrained]);
        assert_eq!((m.acks, m.commits, m.probe_spawns), (1, 1, 0));
        assert_eq!(m.discovery, ScaleDiscovery::Idle);
    }

    #[test]
    fn withheld_scale_probes_after_drain_then_authoritative_corrects() {
        let mut m = Model::new();
        m.run(&[Ev::Configure, Ev::BatchDrained]);
        // Hyprland style: nothing else in the batch, so exactly one probe.
        assert_eq!(m.probe_spawns, 1);
        assert_eq!(m.commits, 0);
        m.run(&[Ev::ProbeCompletes]);
        assert_eq!((m.acks, m.commits), (1, 1));
        assert_eq!(m.scale, Some(ScaleProvenance::Provisional));
        // The authoritative scale after map displaces the provisional one and
        // re-presents without a new configure (and without a new ack).
        m.run(&[Ev::PreferredScale]);
        assert_eq!(m.scale, Some(ScaleProvenance::Authoritative));
        assert_eq!((m.acks, m.commits), (1, 2));
    }

    #[test]
    fn probe_failure_presents_with_unit_fallback() {
        // A failed probe feeds the unit scale through the identical path, so
        // startup still completes.
        let mut m = Model::new();
        m.run(&[Ev::Configure, Ev::BatchDrained, Ev::ProbeCompletes]);
        assert_eq!((m.acks, m.commits), (1, 1));
        assert_eq!(m.scale, Some(ScaleProvenance::Provisional));
    }

    #[test]
    fn late_probe_result_never_clobbers_authoritative_scale() {
        let mut m = Model::new();
        m.run(&[
            Ev::Configure,
            Ev::BatchDrained,
            Ev::PreferredScale,
            Ev::ProbeCompletes,
        ]);
        assert_eq!(m.scale, Some(ScaleProvenance::Authoritative));
    }

    #[test]
    fn no_fractional_scale_manager_uses_unit_scale_without_discovery() {
        let mut m = Model::new();
        m.run(&[Ev::BootUnitScale, Ev::Configure, Ev::BatchDrained]);
        assert_eq!((m.acks, m.commits, m.probe_spawns), (1, 1, 0));
        assert_eq!(m.discovery, ScaleDiscovery::Idle);
    }

    #[test]
    fn repeated_unchanged_scales_re_present_without_new_ack_or_probe() {
        let mut m = Model::new();
        m.run(&[Ev::PreferredScale, Ev::Configure, Ev::BatchDrained]);
        m.run(&[Ev::PreferredScale, Ev::PreferredScale, Ev::BatchDrained]);
        assert_eq!((m.acks, m.commits, m.probe_spawns), (1, 3, 0));
    }

    #[test]
    fn output_migration_scale_without_configure_re_presents() {
        let mut m = Model::new();
        m.run(&[Ev::PreferredScale, Ev::Configure, Ev::BatchDrained]);
        // Moving to another output delivers only a new preferred_scale.
        m.run(&[Ev::PreferredScale, Ev::BatchDrained]);
        assert_eq!((m.acks, m.commits), (1, 2));
    }

    #[test]
    fn bare_configure_ack_after_map() {
        let mut m = Model::new();
        m.run(&[Ev::PreferredScale, Ev::Configure, Ev::BatchDrained]);
        // A size-less configure (states-only change) still acks and commits.
        m.run(&[Ev::Configure, Ev::BatchDrained]);
        assert_eq!((m.acks, m.commits), (2, 2));
    }

    #[test]
    fn pending_fallback_never_respawns_and_never_blocks_shutdown() {
        let mut m = Model::new();
        m.run(&[Ev::Configure, Ev::BatchDrained]);
        assert_eq!(m.probe_spawns, 1);
        // The probe never completes (wedged compositor): further drains must
        // not spawn again — the loop stays free to exit at shutdown, and the
        // orphaned probe thread owns only its private connection.
        m.run(&[Ev::BatchDrained, Ev::BatchDrained]);
        assert_eq!(m.probe_spawns, 1);
        assert_eq!(m.discovery, ScaleDiscovery::Spawned);
        assert_eq!(m.commits, 0);
    }

    #[test]
    fn callbacks_never_spawn_probes() {
        // Spawning happens only on BatchDrained, whatever callbacks arrive.
        let mut m = Model::new();
        m.run(&[
            Ev::Configure,
            Ev::Configure,
            Ev::PreferredScale,
            Ev::Configure,
        ]);
        assert_eq!(m.probe_spawns, 0);
    }
}

#[cfg(test)]
mod tests {
    use super::presentation::{Inputs, ScaleDiscovery, Step, plan};
    use super::resolve_logical_size;
    use crate::window_state::{WindowMode, WindowSize};
    use std::num::NonZeroI32;

    #[test]
    fn discovery_request_is_idempotent_and_never_downgrades() {
        assert_eq!(ScaleDiscovery::Idle.request(), ScaleDiscovery::Requested);
        assert_eq!(
            ScaleDiscovery::Requested.request(),
            ScaleDiscovery::Requested
        );
        // A spawned probe must not be re-requested by later configures.
        assert_eq!(ScaleDiscovery::Spawned.request(), ScaleDiscovery::Spawned);
    }

    #[test]
    fn discovery_batch_drain_transitions() {
        assert_eq!(
            ScaleDiscovery::Requested.after_batch_drained(true),
            (ScaleDiscovery::Idle, false)
        );
        assert_eq!(
            ScaleDiscovery::Requested.after_batch_drained(false),
            (ScaleDiscovery::Spawned, true)
        );
        for known in [false, true] {
            assert_eq!(
                ScaleDiscovery::Idle.after_batch_drained(known),
                (ScaleDiscovery::Idle, false)
            );
            assert_eq!(
                ScaleDiscovery::Spawned.after_batch_drained(known),
                (ScaleDiscovery::Spawned, false)
            );
        }
    }

    fn inputs(mapped: bool, pending_ack: bool, scale_known: bool, size: bool) -> Inputs {
        Inputs {
            mapped,
            pending_ack,
            scale_known,
            size: size.then(|| WindowSize::new(1280, 720).unwrap()),
        }
    }

    #[test]
    fn no_configure_and_unmapped_waits() {
        // Whatever else is known, nothing may happen before the first configure.
        for scale_known in [false, true] {
            for size in [false, true] {
                assert_eq!(plan(inputs(false, false, scale_known, size)), Step::Wait);
            }
        }
    }

    #[test]
    fn first_configure_without_scale_discovers() {
        for size in [false, true] {
            assert_eq!(plan(inputs(false, true, false, size)), Step::DiscoverScale);
        }
    }

    #[test]
    fn mapped_without_scale_waits_instead_of_probing() {
        // After map the compositor owns the scale; a re-present must not probe.
        assert_eq!(plan(inputs(true, true, false, true)), Step::Wait);
        assert_eq!(plan(inputs(true, false, false, true)), Step::Wait);
    }

    #[test]
    fn unresolvable_size_waits() {
        assert_eq!(plan(inputs(false, true, true, false)), Step::Wait);
        assert_eq!(plan(inputs(true, false, true, false)), Step::Wait);
    }

    #[test]
    fn presents_once_configured_scaled_and_sized() {
        assert_eq!(plan(inputs(false, true, true, true)), Step::Present);
        assert_eq!(plan(inputs(true, true, true, true)), Step::Present);
        // Re-present without a new configure (scale/size change after map).
        assert_eq!(plan(inputs(true, false, true, true)), Step::Present);
    }

    const NONE: (Option<NonZeroI32>, Option<NonZeroI32>) = (None, None);

    fn pending(w: i32, h: i32) -> (Option<NonZeroI32>, Option<NonZeroI32>) {
        (NonZeroI32::new(w), NonZeroI32::new(h))
    }

    fn size(w: i32, h: i32) -> Option<WindowSize> {
        WindowSize::new(w, h)
    }

    #[test]
    fn maximized_without_compositor_size_defers() {
        assert_eq!(
            resolve_logical_size(NONE, None, size(1280, 720), WindowMode::Maximized),
            None
        );
        assert_eq!(
            resolve_logical_size(NONE, None, size(1280, 720), WindowMode::Fullscreen),
            None
        );
    }

    #[test]
    fn tiled_defers_like_maximized_not_floating() {
        // Tiled is compositor-dictated: without a compositor size it must defer,
        // not fall back to the saved floating size.
        assert_eq!(
            resolve_logical_size(NONE, None, size(1280, 720), WindowMode::Tiled),
            None
        );
        assert!(!WindowMode::Tiled.uses_floating_restore());
    }

    #[test]
    fn floating_without_compositor_size_uses_floating() {
        assert_eq!(
            resolve_logical_size(NONE, None, size(1280, 720), WindowMode::Floating),
            size(1280, 720)
        );
    }

    #[test]
    fn unmaximize_uses_floating_not_stale_cur() {
        assert_eq!(
            resolve_logical_size(NONE, size(1920, 1080), size(800, 600), WindowMode::Floating),
            size(800, 600)
        );
    }

    #[test]
    fn compositor_size_wins_for_every_mode() {
        for mode in [
            WindowMode::Floating,
            WindowMode::Tiled,
            WindowMode::Maximized,
            WindowMode::Fullscreen,
        ] {
            assert_eq!(
                resolve_logical_size(pending(2560, 1440), size(800, 600), size(1280, 720), mode),
                size(2560, 1440)
            );
        }
    }

    #[test]
    fn last_completed_size_bridges_a_bare_ack() {
        assert_eq!(
            resolve_logical_size(
                NONE,
                size(2560, 1440),
                size(1280, 720),
                WindowMode::Maximized
            ),
            size(2560, 1440)
        );
    }
}
