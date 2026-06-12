//! X11 init/cleanup/clamp and helpers for atom interning, ARGB visual
//! discovery, parent geometry queries, and overlay repositioning.

use x11rb::connection::Connection as X11rbConnection;
use x11rb::protocol::shm::ConnectionExt as X11rbShmConnection;
use x11rb::protocol::xproto::{ConnectionExt as X11rbXprotoConnection, Screen, VisualClass};
use x11rb::rust_connection::RustConnection;

use crate::x11_state::{Atoms, MUT, Mutable, X11RB_CONN, is_none_gc, is_none_window};

use jfn_mpv::api::jfn_mpv_get_property_int;

fn paint_name(mode: crate::paint_override::X11PaintOverride) -> &'static str {
    use crate::paint_override::X11PaintOverride as M;
    match mode {
        M::Dmabuf => "dmabuf",
        M::Gpu => "gpu",
        M::Shm => "shm",
    }
}

fn cef_dmabuf_producer_ok() -> bool {
    unsafe {
        jfn_linux_util::dmabuf_probe::jfn_wl_dmabuf_probe(c"x11".as_ptr(), std::ptr::null_mut())
    }
}

fn cef_producer_target() -> jfn_gpu_paint::GpuTarget {
    let drm_render = unsafe {
        jfn_linux_util::dmabuf_probe::cef_render_node(c"x11".as_ptr(), std::ptr::null_mut())
    };
    jfn_gpu_paint::GpuTarget { drm_render }
}

/// Find a 32-bit TrueColor visual.
fn find_argb_visual(screen: &Screen) -> Option<u32> {
    screen
        .allowed_depths
        .iter()
        .filter(|d| d.depth == 32)
        .flat_map(|d| d.visuals.iter())
        .find(|v| v.class == VisualClass::TRUE_COLOR)
        .map(|v| v.visual_id)
}

fn intern_atom(conn: &RustConnection, name: &[u8]) -> u32 {
    conn.intern_atom(false, name)
        .ok()
        .and_then(|cookie| cookie.reply().ok())
        .map(|r| r.atom)
        .unwrap_or(0)
}

pub(crate) const COMPOSITOR_NOT_DETECTED_MSG: &str =
    "X11 compositing manager not detected. CEF overlays will not be transparent";
pub(crate) const COMPOSITOR_DETECTED_MSG: &str = "X11 compositing manager detected";

pub(crate) fn cm_atom_name(screen_num: i32) -> String {
    format!("_NET_WM_CM_S{screen_num}")
}

fn compositor_present(conn: &RustConnection, screen_num: i32) -> bool {
    let atom = intern_atom(conn, cm_atom_name(screen_num).as_bytes());
    match conn.get_selection_owner(atom).map(|c| c.reply()) {
        Ok(Ok(reply)) => reply.owner != x11rb::NONE,
        _ => true,
    }
}

pub(crate) fn query_parent_geometry_x11rb(
    conn: &RustConnection,
    parent: u32,
    root: u32,
) -> Option<(i32, i32, i32, i32)> {
    let geo = conn.get_geometry(parent).ok()?.reply().ok()?;
    let trans = conn
        .translate_coordinates(parent, root, 0, 0)
        .ok()?
        .reply()
        .ok()?;
    Some((
        trans.dst_x as i32,
        trans.dst_y as i32,
        geo.width as i32,
        geo.height as i32,
    ))
}

#[derive(Copy, Clone)]
pub(crate) struct OverlaySnap {
    pub window: u32,
    pub visible: bool,
    /// False on the dmabuf tier once a worker exists: the GPU worker sizes the
    /// window in lockstep, so the geometry thread must not drive size too.
    pub send_size: bool,
    pub state: crate::overlay_fsm::OverlayState,
}

pub(crate) fn snapshot_live_overlays_locked(m: &Mutable) -> Vec<OverlaySnap> {
    m.live
        .iter()
        .filter(|&&s_ptr| !s_ptr.is_null())
        .map(|&s_ptr| unsafe { &*s_ptr })
        .filter(|s| !is_none_window(s.window))
        .map(|s| OverlaySnap {
            window: s.window,
            visible: s.visible,
            send_size: !(m.use_dmabuf && s.gpu_paint_worker.is_some()),
            state: s.fsm_state.unwrap_or_else(|| {
                crate::overlay_fsm::OverlayState::new_mapped(m.parent_fullscreen)
            }),
        })
        .collect()
}

pub(crate) fn store_overlay_state_locked(
    m: &Mutable,
    window: u32,
    state: crate::overlay_fsm::OverlayState,
) {
    for &s_ptr in &m.live {
        if s_ptr.is_null() {
            continue;
        }
        let s = unsafe { &mut *s_ptr };
        if s.window == window {
            s.fsm_state = Some(state);
            return;
        }
    }
}

pub(crate) fn set_parent_geometry_locked(m: &mut Mutable, px: i32, py: i32, pw: i32, ph: i32) {
    m.parent_x = px;
    m.parent_y = py;
    m.pw = pw;
    m.ph = ph;
}

pub fn hide_all_live_locked(m: &Mutable) {
    let Some(conn) = crate::x11_state::x11rb_conn() else {
        return;
    };
    for &s_ptr in &m.live {
        if s_ptr.is_null() {
            continue;
        }
        let s = unsafe { &*s_ptr };
        if !is_none_window(s.window) {
            let _ = conn.unmap_window(s.window);
        }
    }
    let _ = conn.flush();
}

/// Platform init. Opens X11 connections, finds the ARGB visual,
/// interns atoms, queries parent geometry, and starts the input thread.
pub fn init() -> bool {
    let mut wid: i64 = 0;
    let name = c"window-id";
    let rc = unsafe { jfn_mpv_get_property_int(name.as_ptr(), &mut wid) };
    if rc < 0 || wid <= 0 {
        eprintln!("[x11] failed to get window-id from mpv");
        return false;
    }
    let parent = wid as u32;

    let (x11rb_conn, screen_num) = match RustConnection::connect(None) {
        Ok((conn, screen_num)) => (std::sync::Arc::new(conn), screen_num as i32),
        Err(e) => {
            eprintln!("[x11] failed to connect x11rb control connection: {e:?}");
            return false;
        }
    };
    if let Err(e) = crate::x11_state::open_xcb_connection() {
        eprintln!("[x11] failed to connect xcb interop/input connection: {e}");
        return false;
    }

    let setup = x11rb_conn.setup();
    let Some(screen) = setup.roots.get(screen_num as usize) else {
        eprintln!("[x11] no screen at index {screen_num}");
        return false;
    };
    let root = screen.root;

    if !compositor_present(&x11rb_conn, screen_num) {
        tracing::error!(target: "Platform", "{COMPOSITOR_NOT_DETECTED_MSG}");
    }

    let argb_depth: u8 = 32;
    let Some(argb_visual) = find_argb_visual(screen) else {
        eprintln!("[x11] no 32-bit ARGB visual found");
        return false;
    };

    let Ok(colormap_id) = x11rb_conn.generate_id() else {
        eprintln!("[x11] failed to allocate colormap id");
        return false;
    };
    let colormap = colormap_id;
    if x11rb_conn
        .create_colormap(
            x11rb::protocol::xproto::ColormapAlloc::NONE,
            colormap_id,
            root,
            argb_visual,
        )
        .is_err()
    {
        eprintln!("[x11] failed to create colormap");
        return false;
    }

    let atoms = Atoms {
        net_wm_window_type: intern_atom(&x11rb_conn, b"_NET_WM_WINDOW_TYPE"),
        net_wm_window_type_normal: intern_atom(&x11rb_conn, b"_NET_WM_WINDOW_TYPE_NORMAL"),
        net_wm_state: intern_atom(&x11rb_conn, b"_NET_WM_STATE"),
        net_wm_state_skip_taskbar: intern_atom(&x11rb_conn, b"_NET_WM_STATE_SKIP_TASKBAR"),
        net_wm_state_skip_pager: intern_atom(&x11rb_conn, b"_NET_WM_STATE_SKIP_PAGER"),
        net_wm_state_fullscreen: intern_atom(&x11rb_conn, b"_NET_WM_STATE_FULLSCREEN"),
        wm_protocols: intern_atom(&x11rb_conn, b"WM_PROTOCOLS"),
        wm_delete_window: intern_atom(&x11rb_conn, b"WM_DELETE_WINDOW"),
        motif_wm_hints: intern_atom(&x11rb_conn, b"_MOTIF_WM_HINTS"),
        net_active_window: intern_atom(&x11rb_conn, b"_NET_ACTIVE_WINDOW"),
    };

    // Verify the MIT-SHM extension is present.
    let shm_ok = x11rb_conn
        .shm_query_version()
        .ok()
        .and_then(|cookie| cookie.reply().ok())
        .is_some();
    if !shm_ok {
        tracing::error!("MIT-SHM extension not available");
        return false;
    }

    let (parent_x, parent_y, pw, ph) =
        query_parent_geometry_x11rb(&x11rb_conn, parent, root).unwrap_or((0, 0, 1, 1));

    // Resolve the paint preference down the dmabuf → gpu → shm chain, where
    // `--platform-paint` only picks the entry tier and an unusable tier degrades
    // to the next.
    use crate::paint_override::X11PaintOverride as Req;
    let requested = crate::paint_override::paint_override();
    let explicit = requested.is_some();
    let want_gpu = !matches!(requested, Some(Req::Shm));
    let want_dmabuf = matches!(requested, None | Some(Req::Dmabuf));
    let (gpu_ctx, gpu_caps, use_dmabuf, resolved) = if want_gpu {
        let target = cef_producer_target();
        let caps = jfn_gpu_paint::GpuContext::probe(target);
        if caps.gpu_available {
            match jfn_gpu_paint::GpuContext::new(target) {
                Ok(c) => {
                    let caps = c.capabilities();
                    // caps.dmabuf_import only proves our Vulkan side can consume;
                    // also probe CEF's producer, broken on NVIDIA proprietary X11.
                    if want_dmabuf
                        && caps.dmabuf_import
                        && caps.dmabuf_device_matched
                        && cef_dmabuf_producer_ok()
                    {
                        tracing::info!("paint: dmabuf import");
                        (Some(c), caps, true, Req::Dmabuf)
                    } else {
                        tracing::info!("paint: Vulkan pixel-upload");
                        (Some(c), caps, false, Req::Gpu)
                    }
                }
                Err(e) => {
                    tracing::info!("paint: Vulkan init failed: {e}; using SHM");
                    (None, jfn_gpu_paint::Capabilities::NONE, false, Req::Shm)
                }
            }
        } else {
            tracing::info!("paint: no Vulkan adapter; using SHM");
            (None, jfn_gpu_paint::Capabilities::NONE, false, Req::Shm)
        }
    } else {
        tracing::info!("paint: using SHM");
        (None, jfn_gpu_paint::Capabilities::NONE, false, Req::Shm)
    };
    if explicit
        && let Some(req) = requested
        && req != resolved
    {
        tracing::warn!(
            "--platform-paint={} unavailable; using {}",
            paint_name(req),
            paint_name(resolved)
        );
    }

    // Populate the global mutable state.
    {
        let mut g = MUT.lock();
        *g = Some(Mutable {
            screen_num,
            root,
            argb_visual,
            argb_depth,
            colormap,
            parent,
            parent_x,
            parent_y,
            pw,
            ph,
            parent_fullscreen: false,
            atoms,
            live: Vec::new(),
            gpu_ctx,
            gpu_caps,
            use_dmabuf,
            gate: jfn_compositor_core::transition::TransitionGate::new(),
        });
    }

    if X11RB_CONN.set(x11rb_conn).is_err() {
        eprintln!("[x11] x11rb connection already initialized");
        return false;
    }

    if let Some(s) = crate::scale::query_display_scale() {
        jfn_platform_abi::scale_push_boot(s);
        jfn_platform_abi::scale_push(s);
    }

    crate::input_lifecycle::start(parent);
    crate::geometry::start(parent, root);

    eprintln!("[x11] platform initialized (parent=0x{:x})", parent);
    true
}

pub fn cleanup() {
    // Defensively unmap any straggler surface windows.
    {
        let g = MUT.lock();
        if let Some(m) = g.as_ref() {
            hide_all_live_locked(m);
        }
    }

    jfn_linux_util::idle_inhibit::cleanup();
    crate::geometry::cleanup();
    crate::input_lifecycle::cleanup();

    // Free any surface that outlived Browsers (defensive).
    if let Some(conn) = crate::x11_state::x11rb_conn() {
        let mut g = MUT.lock();
        if let Some(m) = g.as_mut() {
            for &s_ptr in &m.live {
                if s_ptr.is_null() {
                    continue;
                }
                let s = unsafe { &mut *s_ptr };
                if let Some(worker) = s.gpu_paint_worker.take() {
                    worker.shutdown();
                }
                if let Some(worker) = s.shm_paint_worker.take() {
                    worker.shutdown();
                }
                if !is_none_gc(s.gc) {
                    let _ = conn.free_gc(s.gc);
                }
                if !is_none_window(s.window) {
                    let _ = conn.destroy_window(s.window);
                }
                drop(unsafe { Box::from_raw(s_ptr) });
            }
            m.live.clear();
            if m.colormap != 0 {
                let _ = conn.free_colormap(m.colormap);
            }
        }
        let _ = conn.flush();
    }
}

/// Clamp saved window geometry to the primary screen extent. Runs before
/// `init()` so it opens its own short-lived connection.
pub fn clamp_window_geometry(w: &mut i32, h: &mut i32) {
    let Ok((conn, screen_num)) = RustConnection::connect(None) else {
        return;
    };
    let Some(root) = conn.setup().roots.get(screen_num) else {
        return;
    };
    let sw = root.width_in_pixels as i32;
    let sh = root.height_in_pixels as i32;
    if sw > 0 && *w > sw {
        *w = sw;
    }
    if sh > 0 && *h > sh {
        *h = sh;
    }
}
