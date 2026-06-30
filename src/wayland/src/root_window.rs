use std::ffi::{c_int, c_void};
use std::os::fd::{AsFd, AsRawFd};
use std::sync::Arc;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use std::thread::{self, JoinHandle};

use parking_lot::Mutex;

use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::{
    wl_buffer::WlBuffer,
    wl_compositor::WlCompositor,
    wl_registry::WlRegistry,
    wl_seat::WlSeat,
    wl_shm::{Format, WlShm},
    wl_shm_pool::WlShmPool,
    wl_surface::WlSurface,
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

use memmap2::MmapOptions;

const APP_ID: &str = "org.jellyfin.JellyfinDesktop";
const TITLE: &str = "Jellyfin Desktop";

// Background behind the video/overlay, matching kBgColor (0x101010).
const BG: [u8; 3] = [0x10, 0x10, 0x10];

const DEFAULT_W: i32 = 1280;
const DEFAULT_H: i32 = 720;

const STATE_MAXIMIZED: u32 = 1;
const STATE_FULLSCREEN: u32 = 2;
const STATE_SUSPENDED: u32 = 9;

// Wire values set by `mpv_host::set_decorations`: 1=Csd, 2=Server,
// 3=ServerThemed. 0 = unset (treated as server-side).
const DECO_CSD: u32 = 1;
static DECO_MODE: AtomicU32 = AtomicU32::new(0);

pub(crate) fn set_decorations(mode: u32) {
    DECO_MODE.store(mode, Ordering::Release);
}

static BOOT_W: AtomicU32 = AtomicU32::new(DEFAULT_W as u32);
static BOOT_H: AtomicU32 = AtomicU32::new(DEFAULT_H as u32);
static BOOT_MAX: AtomicBool = AtomicBool::new(false);

pub(crate) fn set_boot_geometry(w: i32, h: i32, maximized: bool) {
    if w > 0 && h > 0 {
        BOOT_W.store(w as u32, Ordering::Release);
        BOOT_H.store(h as u32, Ordering::Release);
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
    shm: WlShm,
    viewport: Option<WpViewport>,
    bg_buffer: Option<WlBuffer>,
    bg: [u8; 3],
    // Held alive so the compositor keeps delivering preferred_scale.
    #[allow(dead_code)]
    frac_mgr: Option<WpFractionalScaleManagerV1>,
    #[allow(dead_code)]
    frac_scale: Option<WpFractionalScaleV1>,
    #[allow(dead_code)]
    decoration: Option<ZxdgToplevelDecorationV1>,

    cur_w: i32,
    cur_h: i32,
    // Latest size from xdg_toplevel.configure (0 = compositor defers to us).
    pending_w: i32,
    pending_h: i32,
    // Scale numerator over 120 (120 = 1.0).
    scale_120: u32,
    fullscreen: bool,
    maximized: bool,
    suspended: bool,
    boot_w: i32,
    boot_h: i32,
    mapped: bool,
}

impl RootState {
    fn configure(&mut self, serial: u32) {
        self.xdg_surface.ack_configure(serial);

        let w = if self.pending_w > 0 {
            self.pending_w
        } else if self.cur_w > 0 {
            self.cur_w
        } else {
            self.boot_w
        }
        .max(1);
        let h = if self.pending_h > 0 {
            self.pending_h
        } else if self.cur_h > 0 {
            self.cur_h
        } else {
            self.boot_h
        }
        .max(1);

        // Geometry + background are cached on the root; the single root commit
        // that presents them is issued by `present_transaction` below, together
        // with the overlay/video subtree — never as a standalone commit here.
        self.xdg_surface.set_window_geometry(0, 0, w, h);
        self.fill_background(w, h);
        self.cur_w = w;
        self.cur_h = h;
        if !self.mapped {
            tracing::info!(target: "Main", "root window: first configure {w}x{h} (app toplevel is live)");
        }
        self.mapped = true;

        // mpv's synthesized configure + host-overlay resize use logical size
        // (xdg/viewport coordinate space); mpv applies scale itself.
        crate::mpv_proxy::set_window_size(w, h);
        // The host (CEF buffers, boot gate, OSD) works in physical pixels; the
        // overlay/mpv extent mirrors the logical size. Both are passed exactly —
        // no consumer re-derives one from the other.
        let (pw, ph) = self.physical(w, h);
        crate::window_state::feed_window_state(w, h, pw, ph, self.fullscreen, self.maximized);

        // One owner-issued commit applies geometry + overlay + video atomically.
        self.present_transaction();
    }

    fn present_transaction(&mut self) {
        if !self.mapped {
            return;
        }
        self.surface.commit();
        let _ = self.conn.flush();
    }

    fn physical(&self, lw: i32, lh: i32) -> (i32, i32) {
        let s = i64::from(self.scale_120);
        (
            ((i64::from(lw) * s + 60) / 120) as i32,
            ((i64::from(lh) * s + 60) / 120) as i32,
        )
    }

    fn fill_background(&mut self, w: i32, h: i32) {
        if let Some(vp) = &self.viewport {
            vp.set_destination(w, h);
        }
        if self.bg_buffer.is_none() {
            self.bg_buffer = self.create_solid_buffer();
        }
        if let Some(buf) = &self.bg_buffer {
            self.surface.attach(Some(buf), 0, 0);
            self.surface.damage_buffer(0, 0, i32::MAX, i32::MAX);
        }
    }

    fn create_solid_buffer(&self) -> Option<WlBuffer> {
        let fd = crate::wl_state::memfd_anon("root-bg", 4)?;
        {
            let mut mmap = unsafe { MmapOptions::new().len(4).map_mut(&fd) }.ok()?;
            // ARGB8888 little-endian byte order = [B, G, R, A].
            mmap[0] = self.bg[2];
            mmap[1] = self.bg[1];
            mmap[2] = self.bg[0];
            mmap[3] = 0xFF;
        }
        let pool: WlShmPool = self.shm.create_pool(fd.as_fd(), 4, &self.qh, ());
        let buf = pool.create_buffer(0, 1, 1, 4, Format::Argb8888, &self.qh, ());
        pool.destroy();
        Some(buf)
    }
}

static STARTED: AtomicBool = AtomicBool::new(false);

static ROOT_SURFACE_PTR: AtomicUsize = AtomicUsize::new(0);

pub(crate) fn root_surface_ptr() -> *mut std::ffi::c_void {
    ROOT_SURFACE_PTR.load(Ordering::Acquire) as *mut std::ffi::c_void
}

struct Controls {
    conn: Connection,
    toplevel: XdgToplevel,
    seat: Option<WlSeat>,
}
static CONTROLS: OnceLock<Controls> = OnceLock::new();

pub(crate) fn start_move() {
    if let Some(c) = CONTROLS.get()
        && let Some(seat) = &c.seat
    {
        c.toplevel._move(seat, crate::input::last_button_serial());
        let _ = c.conn.flush();
    }
}

pub(crate) fn start_resize(edge: u32) {
    if let Some(c) = CONTROLS.get()
        && let Some(seat) = &c.seat
        && let Ok(e) = xdg_toplevel::ResizeEdge::try_from(edge)
    {
        c.toplevel
            .resize(seat, crate::input::last_button_serial(), e);
        let _ = c.conn.flush();
    }
}

pub(crate) fn set_fullscreen(on: bool) {
    if let Some(c) = CONTROLS.get() {
        if on {
            c.toplevel.set_fullscreen(None);
        } else {
            c.toplevel.unset_fullscreen();
        }
        let _ = c.conn.flush();
    }
}

pub(crate) fn set_maximized(on: bool) {
    if let Some(c) = CONTROLS.get() {
        if on {
            c.toplevel.set_maximized();
        } else {
            c.toplevel.unset_maximized();
        }
        let _ = c.conn.flush();
    }
}

pub(crate) fn set_minimized() {
    if let Some(c) = CONTROLS.get() {
        c.toplevel.set_minimized();
        let _ = c.conn.flush();
    }
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

    pub(crate) fn create_shm_buffer(&self, pixels: &[u8], w: i32, h: i32) -> Option<WlBuffer> {
        let stride = w.checked_mul(4)?;
        let size = stride.checked_mul(h)?;
        if size <= 0 || pixels.len() < size as usize {
            return None;
        }
        let fd = crate::wl_state::memfd_anon("menu-sw", size as usize)?;
        {
            let mut mmap = unsafe { MmapOptions::new().len(size as usize).map_mut(&fd) }.ok()?;
            mmap.copy_from_slice(&pixels[..size as usize]);
        }
        let pool: WlShmPool = self.shm.create_pool(fd.as_fd(), size, &self.qh, ());
        let buf = pool.create_buffer(0, w, h, stride, Format::Argb8888, &self.qh, ());
        pool.destroy();
        Some(buf)
    }

    pub(crate) fn flush(&self) {
        let _ = self.conn.flush();
    }
}

// Ties a configure/popup_done back to the menu generation that owns it, so a
// late event from a torn-down popup is ignored.
#[derive(Clone, Copy)]
struct PopupRole {
    generation: u32,
}

struct PopupRoleObjs {
    xdg: Option<XdgSurface>,
    popup: Option<XdgPopup>,
}
static POPUP_ROLE: Mutex<PopupRoleObjs> = Mutex::new(PopupRoleObjs {
    xdg: None,
    popup: None,
});

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
pub(crate) fn popup_create(generation: u32, x: i32, y: i32, w: i32, h: i32, surface: &WlSurface) {
    let Some(shell) = popup_shell() else {
        return;
    };
    popup_destroy();
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
    let mut role = POPUP_ROLE.lock();
    role.xdg = Some(xdg);
    role.popup = Some(popup);
}

/// Requires the popup to already be mapped.
pub(crate) fn popup_reposition(x: i32, y: i32, w: i32, h: i32) {
    let Some(shell) = popup_shell() else {
        return;
    };
    let popup = POPUP_ROLE.lock().popup.clone();
    let Some(popup) = popup else {
        return;
    };
    let positioner = build_menu_positioner(shell, x, y, w, h);
    popup.reposition(&positioner, 0);
    positioner.destroy();
    shell.flush();
}

/// Destroys only the popup role objects, not the menu wl_surface — that surface
/// is persistent (owned by crate::popup) and re-roled on the next open.
pub(crate) fn popup_destroy() {
    let (popup, xdg) = {
        let mut role = POPUP_ROLE.lock();
        (role.popup.take(), role.xdg.take())
    };
    if let Some(p) = popup {
        p.destroy();
    }
    if let Some(x) = xdg {
        x.destroy();
    }
    if let Some(shell) = popup_shell() {
        shell.flush();
    }
}

// High bit marks "set"; the low 24 bits are RGB. Applied on the dispatch thread,
// which owns the surface, so commits don't race the configure handler.
static PENDING_BG: AtomicU32 = AtomicU32::new(0);
const BG_SET: u32 = 1 << 24;

pub(crate) fn set_background_color(r: u8, g: u8, b: u8) {
    let rgb = (u32::from(r) << 16) | (u32::from(g) << 8) | u32::from(b);
    PENDING_BG.store(BG_SET | rgb, Ordering::Release);
    if let Some(t) = ROOT_THREAD.get() {
        let v: u64 = 1;
        unsafe { libc::write(t.wake_fd, &v as *const u64 as *const c_void, 8) };
    }
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
    if let Some(t) = ROOT_THREAD.get() {
        let v: u64 = 1;
        unsafe { libc::write(t.wake_fd, &v as *const u64 as *const c_void, 8) };
    }
}

#[cfg(feature = "kde-palette")]
struct Palette {
    conn: Connection,
    palette: OrgKdeKwinServerDecorationPalette,
}
#[cfg(feature = "kde-palette")]
static PALETTE: OnceLock<Palette> = OnceLock::new();

#[cfg(feature = "kde-palette")]
pub(crate) fn set_titlebar_palette(path: &std::path::Path) {
    if let Some(p) = PALETTE.get()
        && let Some(s) = path.to_str()
    {
        p.palette.set_palette(s.to_owned());
        let _ = p.conn.flush();
    }
}

// Teardown handle for the dispatch thread. Without it the thread sits in
// `poll(-1)` holding a `wl_display` read barrier; when no video ever played the
// display is quiet, so the barrier is never released and mpv's VO-teardown
// roundtrip hangs forever. `cleanup` signals + joins before that roundtrip.
struct RootThread {
    stop: Arc<AtomicBool>,
    wake_fd: c_int,
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
    let v: u64 = 1;
    unsafe { libc::write(t.wake_fd, &v as *const u64 as *const c_void, 8) };
    if let Some(h) = t.handle.lock().take() {
        let _ = h.join();
        unsafe { libc::close(t.wake_fd) };
    }
}

fn vo_display() -> Option<*mut std::ffi::c_void> {
    let d = crate::app_conn::app_display();
    (!d.is_null()).then_some(d)
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

    let backend = unsafe { wayland_backend::client::Backend::from_foreign_display(display.cast()) };
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
    // Publish the raw wl_proxy so wl_state can parent its CEF overlay under this
    // surface: same libwayland wl_display, but a different wayland-client Backend,
    // so it must be reconstructed there via ObjectId::from_ptr.
    ROOT_SURFACE_PTR.store(surface.id().as_ptr() as usize, Ordering::Release);
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
        // No preferred_scale will ever arrive, so satisfy the boot scale gate at
        // 1.0 — otherwise it waits forever.
        tracing::warn!(target: "Main", "root window: no wp_fractional_scale_manager_v1; assuming scale 1.0");
        crate::window_state::feed_scale(120);
    }

    // Request server/client-side decorations to match the configured mode.
    // Without an explicit request a compositor's default (KWin: server-side,
    // sway: none) leaves the window with no titlebar.
    let deco_mgr: Option<ZxdgDecorationManagerV1> = globals.bind(&qh, 1..=1, ()).ok();
    let decoration = deco_mgr.as_ref().map(|mgr| {
        let dec = mgr.get_toplevel_decoration(&toplevel, &qh, ());
        let mode = if DECO_MODE.load(Ordering::Acquire) == DECO_CSD {
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
    if let Ok(mgr) = globals.bind::<OrgKdeKwinServerDecorationPaletteManager, _, _>(&qh, 1..=1, ())
    {
        let palette = mgr.create(&surface, &qh, ());
        let _ = PALETTE.set(Palette {
            conn: conn.clone(),
            palette,
        });
    }

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

    let _ = CONTROLS.set(Controls {
        conn: conn.clone(),
        toplevel: toplevel.clone(),
        seat,
    });

    // Roleless commit to elicit the first xdg_surface.configure.
    surface.commit();
    let _ = conn.flush();

    let state = RootState {
        conn: conn.clone(),
        qh,
        surface,
        xdg_surface,
        toplevel,
        shm: shm.clone(),
        viewport,
        bg_buffer: None,
        bg: pending_bg().unwrap_or(BG),
        frac_mgr,
        frac_scale,
        decoration,
        cur_w: 0,
        cur_h: 0,
        pending_w: 0,
        pending_h: 0,
        scale_120: 120,
        fullscreen: false,
        maximized: boot_max,
        suspended: false,
        boot_w,
        boot_h,
        mapped: false,
    };

    let wake_fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    if wake_fd < 0 {
        tracing::error!(target: "Main", "root window: eventfd failed");
        return;
    }
    let stop = Arc::new(AtomicBool::new(false));
    let stop_thread = stop.clone();
    match thread::Builder::new()
        .name("wl-root".into())
        .spawn(move || root_loop(queue, state, wake_fd, stop_thread))
    {
        Ok(handle) => {
            let _ = ROOT_THREAD.set(RootThread {
                stop,
                wake_fd,
                handle: Mutex::new(Some(handle)),
            });
        }
        Err(e) => {
            unsafe { libc::close(wake_fd) };
            tracing::error!(target: "Main", "root window: thread spawn: {e}");
        }
    }
}

// Coordinates with the other readers on the shared fd via prepare_read + poll
// (a blocking dispatch here would deadlock them). A wake eventfd lets `cleanup`
// break the poll so the read barrier is released at shutdown.
fn root_loop(
    mut queue: EventQueue<RootState>,
    mut state: RootState,
    wake_fd: c_int,
    stop: Arc<AtomicBool>,
) {
    let conn = state.conn.clone();
    let fd = conn.as_fd().as_raw_fd();
    loop {
        if queue.dispatch_pending(&mut state).is_err() {
            break;
        }
        let _ = conn.flush();

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
            let mut buf = [0u8; 64];
            loop {
                let n = unsafe { libc::read(wake_fd, buf.as_mut_ptr() as *mut c_void, buf.len()) };
                if n <= 0 {
                    break;
                }
            }
            if stop.load(Ordering::Relaxed) {
                break;
            }
            if let Some(bg) = pending_bg()
                && bg != state.bg
            {
                state.bg = bg;
                state.bg_buffer = None;
                if state.cur_w > 0 && state.cur_h > 0 {
                    let (w, h) = (state.cur_w, state.cur_h);
                    state.fill_background(w, h);
                    // Apply via the single owner commit, not a standalone one.
                    PENDING_PRESENT.store(true, Ordering::Release);
                }
            }
            if PENDING_PRESENT.swap(false, Ordering::Acquire) {
                state.present_transaction();
            }
        }
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
            state.configure(serial);
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
                state.pending_w = width;
                state.pending_h = height;
                let (mut fs, mut max, mut suspended) = (false, false, false);
                for chunk in states.chunks_exact(4) {
                    match u32::from_ne_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]) {
                        STATE_FULLSCREEN => fs = true,
                        STATE_MAXIMIZED => max = true,
                        STATE_SUSPENDED => suspended = true,
                        _ => {}
                    }
                }
                state.fullscreen = fs;
                state.maximized = max;
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
            state.scale_120 = scale;
            crate::window_state::feed_scale(scale as i32);
            // Scale can change without a resize (e.g. moved to another output);
            // re-feed the host with the new physical size for the current logical.
            if state.cur_w > 0 && state.cur_h > 0 {
                let (pw, ph) = state.physical(state.cur_w, state.cur_h);
                crate::window_state::feed_window_state(
                    state.cur_w,
                    state.cur_h,
                    pw,
                    ph,
                    state.fullscreen,
                    state.maximized,
                );
                state.present_transaction();
            }
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
            popup_destroy();
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
    WlBuffer,
    WpViewporter,
    WpViewport,
    WpFractionalScaleManagerV1,
    ZxdgDecorationManagerV1,
    WlSeat,
    XdgPositioner,
);

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
