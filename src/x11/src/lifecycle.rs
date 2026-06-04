//! X11 init/cleanup/clamp and helpers for atom interning, ARGB visual
//! discovery, parent geometry queries, and overlay repositioning.

use xcb::{Xid, XidNew, x};

use crate::x11_state::{Atoms, CONN, MUT, Mutable, is_none_gc, is_none_window};

use jfn_mpv::api::jfn_mpv_get_property_int;

fn paint_name(mode: crate::paint_override::X11PaintOverride) -> &'static str {
    use crate::paint_override::X11PaintOverride as M;
    match mode {
        M::Dmabuf => "dmabuf",
        M::Gpu => "gpu",
        M::Shm => "shm",
    }
}

/// Find a 32-bit TrueColor visual.
fn find_argb_visual(screen: &x::Screen) -> Option<x::Visualid> {
    screen
        .allowed_depths()
        .filter(|d| d.depth() == 32)
        .flat_map(|d| d.visuals())
        .find(|v| v.class() == x::VisualClass::TrueColor)
        .map(|v| v.visual_id())
}

fn intern_atom(conn: &xcb::Connection, name: &[u8]) -> x::Atom {
    let cookie = conn.send_request(&x::InternAtom {
        only_if_exists: false,
        name,
    });
    conn.wait_for_reply(cookie)
        .map(|r| r.atom())
        .unwrap_or(x::ATOM_NONE)
}

/// Query the parent window's absolute screen position + size. Returns
/// None on protocol failure.
pub fn query_parent_geometry(
    conn: &xcb::Connection,
    parent: x::Window,
    root: x::Window,
) -> Option<(i32, i32, i32, i32)> {
    let geo_cookie = conn.send_request(&x::GetGeometry {
        drawable: x::Drawable::Window(parent),
    });
    let geo = conn.wait_for_reply(geo_cookie).ok()?;
    let trans_cookie = conn.send_request(&x::TranslateCoordinates {
        src_window: parent,
        dst_window: root,
        src_x: 0,
        src_y: 0,
    });
    let trans = conn.wait_for_reply(trans_cookie).ok()?;
    Some((
        trans.dst_x() as i32,
        trans.dst_y() as i32,
        geo.width() as i32,
        geo.height() as i32,
    ))
}

#[derive(Copy, Clone)]
pub(crate) struct OverlaySnap {
    pub window: x::Window,
    pub visible: bool,
    /// False on the dmabuf tier once a worker exists: the GPU worker sizes the
    /// window in lockstep, so the geometry thread must not drive size too.
    pub send_size: bool,
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
        })
        .collect()
}

pub(crate) fn set_parent_geometry_locked(m: &mut Mutable, px: i32, py: i32, pw: i32, ph: i32) {
    m.parent_x = px;
    m.parent_y = py;
    m.pw = pw;
    m.ph = ph;
}

pub(crate) fn reposition_overlays(
    conn: &xcb::Connection,
    px: i32,
    py: i32,
    pw: i32,
    ph: i32,
    snaps: &[OverlaySnap],
) {
    for s in snaps {
        if !s.visible {
            continue;
        }
        let mut value_list = vec![x::ConfigWindow::X(px), x::ConfigWindow::Y(py)];
        if s.send_size {
            value_list.push(x::ConfigWindow::Width(pw as u32));
            value_list.push(x::ConfigWindow::Height(ph as u32));
        }
        conn.send_request(&x::ConfigureWindow {
            window: s.window,
            value_list: &value_list,
        });
    }
    let _ = conn.flush();
}

/// Map every visible overlay. Lock-free; run after releasing `MUT`.
pub(crate) fn map_overlays(conn: &xcb::Connection, snaps: &[OverlaySnap]) {
    for s in snaps {
        if s.visible {
            conn.send_request(&x::MapWindow { window: s.window });
        }
    }
    let _ = conn.flush();
}

/// Unmap every overlay. Lock-free; run after releasing `MUT`.
pub(crate) fn unmap_overlays(conn: &xcb::Connection, snaps: &[OverlaySnap]) {
    for s in snaps {
        conn.send_request(&x::UnmapWindow { window: s.window });
    }
    let _ = conn.flush();
}

/// Re-raise the overlays above `parent` in `snaps` (z-)order. A WM may drop the
/// transient overlays below the parent on a fullscreen transition; re-asserting
/// the stack on settle keeps them visible.
pub(crate) fn restack_overlays_above(
    conn: &xcb::Connection,
    parent: x::Window,
    snaps: &[OverlaySnap],
) {
    let mut prev = parent;
    for s in snaps {
        if s.window == prev {
            continue;
        }
        conn.send_request(&x::ConfigureWindow {
            window: s.window,
            value_list: &[
                x::ConfigWindow::Sibling(prev),
                x::ConfigWindow::StackMode(x::StackMode::Above),
            ],
        });
        prev = s.window;
    }
    let _ = conn.flush();
}

/// Ask the WM to make `parent` the active window again. Restoring from the
/// taskbar re-maps the transient overlays on top of the parent, displacing the
/// WM's active window off mpv; without re-asserting it the taskbar's
/// minimize/activate toggle stalls. Source-indication 2 (pager) so
/// focus-stealing prevention still honors the request.
pub(crate) fn activate_parent(
    conn: &xcb::Connection,
    root: x::Window,
    parent: x::Window,
    net_active_window: x::Atom,
) {
    let ev = x::ClientMessageEvent::new(
        parent,
        net_active_window,
        x::ClientMessageData::Data32([2, 0, 0, 0, 0]),
    );
    conn.send_request(&x::SendEvent {
        propagate: false,
        destination: x::SendEventDest::Window(root),
        event_mask: x::EventMask::SUBSTRUCTURE_NOTIFY | x::EventMask::SUBSTRUCTURE_REDIRECT,
        event: &ev,
    });
    let _ = conn.flush();
}

pub fn hide_all_live_locked(conn: &xcb::Connection, m: &Mutable) {
    for &s_ptr in &m.live {
        if s_ptr.is_null() {
            continue;
        }
        let s = unsafe { &*s_ptr };
        if !is_none_window(s.window) {
            conn.send_request(&x::UnmapWindow { window: s.window });
        }
    }
    let _ = conn.flush();
}

/// Platform init. Opens the xcb connection, finds the ARGB visual,
/// interns atoms, queries parent geometry, and starts the input thread.
pub fn init() -> bool {
    let mut wid: i64 = 0;
    let name = c"window-id";
    let rc = unsafe { jfn_mpv_get_property_int(name.as_ptr(), &mut wid) };
    if rc < 0 || wid <= 0 {
        eprintln!("[x11] failed to get window-id from mpv");
        return false;
    }
    let parent = x::Window::new(wid as u32);

    let (conn, screen_num) = match xcb::Connection::connect(None) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("[x11] failed to connect: {e:?}");
            return false;
        }
    };
    let conn = std::sync::Arc::new(conn);

    let root = {
        let setup = conn.get_setup();
        let Some(screen) = setup.roots().nth(screen_num as usize) else {
            eprintln!("[x11] no screen at index {screen_num}");
            return false;
        };
        screen.root()
    };

    let setup = conn.get_setup();
    let Some(screen) = setup.roots().nth(screen_num as usize) else {
        eprintln!("[x11] no screen at index {screen_num}");
        return false;
    };

    let argb_depth: u8 = 32;
    let Some(argb_visual) = find_argb_visual(screen) else {
        eprintln!("[x11] no 32-bit ARGB visual found");
        return false;
    };

    let colormap: x::Colormap = conn.generate_id();
    conn.send_request(&x::CreateColormap {
        alloc: x::ColormapAlloc::None,
        mid: colormap,
        window: root,
        visual: argb_visual,
    });

    let atoms = Atoms {
        net_wm_window_type: intern_atom(&conn, b"_NET_WM_WINDOW_TYPE"),
        net_wm_window_type_utility: intern_atom(&conn, b"_NET_WM_WINDOW_TYPE_UTILITY"),
        net_wm_state: intern_atom(&conn, b"_NET_WM_STATE"),
        net_wm_state_skip_taskbar: intern_atom(&conn, b"_NET_WM_STATE_SKIP_TASKBAR"),
        net_wm_state_skip_pager: intern_atom(&conn, b"_NET_WM_STATE_SKIP_PAGER"),
        wm_protocols: intern_atom(&conn, b"WM_PROTOCOLS"),
        wm_delete_window: intern_atom(&conn, b"WM_DELETE_WINDOW"),
        motif_wm_hints: intern_atom(&conn, b"_MOTIF_WM_HINTS"),
        net_active_window: intern_atom(&conn, b"_NET_ACTIVE_WINDOW"),
    };

    // Verify the MIT-SHM extension is present.
    let shm_cookie = conn.send_request(&xcb::shm::QueryVersion {});
    if conn.wait_for_reply(shm_cookie).is_err() {
        tracing::error!("MIT-SHM extension not available");
        return false;
    }

    let (parent_x, parent_y, pw, ph) =
        query_parent_geometry(&conn, parent, root).unwrap_or((0, 0, 1, 1));

    // Resolve the paint preference down the dmabuf → gpu → shm chain, where
    // `--platform-paint` only picks the entry tier and an unusable tier degrades
    // to the next.
    use crate::paint_override::X11PaintOverride as Req;
    let requested = crate::paint_override::paint_override();
    let explicit = requested.is_some();
    let want_gpu = !matches!(requested, Some(Req::Shm));
    let want_dmabuf = matches!(requested, None | Some(Req::Dmabuf));
    let (gpu_ctx, gpu_caps, use_dmabuf, resolved) = if want_gpu {
        let caps = jfn_gpu_paint::GpuContext::probe();
        if caps.gpu_available {
            match jfn_gpu_paint::GpuContext::new() {
                Ok(c) => {
                    let caps = c.capabilities();
                    if want_dmabuf && caps.dmabuf_import {
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
    if explicit && requested != Some(resolved) {
        tracing::warn!(
            "--platform-paint={} unavailable; using {}",
            paint_name(requested.unwrap()),
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
            cached_scale: 1.0,
            atoms,
            live: Vec::new(),
            gpu_ctx,
            gpu_caps,
            use_dmabuf,
            gate: jfn_compositor_core::transition::TransitionGate::new(),
        });
    }

    if CONN.set(conn.clone()).is_err() {
        eprintln!("[x11] connection already initialized");
        return false;
    }

    crate::input_lifecycle::start(conn.clone(), parent);
    crate::geometry::start(parent, root);

    eprintln!(
        "[x11] platform initialized (parent=0x{:x})",
        parent.resource_id()
    );
    true
}

pub fn cleanup() {
    // Defensively unmap any straggler surface windows.
    if let Some(conn) = crate::x11_state::conn() {
        let g = MUT.lock();
        if let Some(m) = g.as_ref() {
            hide_all_live_locked(&conn, m);
        }
    }

    jfn_linux_util::idle_inhibit::cleanup();
    crate::geometry::cleanup();
    crate::input_lifecycle::cleanup();

    // Free any surface that outlived Browsers (defensive).
    if let Some(conn) = crate::x11_state::conn() {
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
                    conn.send_request(&x::FreeGc { gc: s.gc });
                }
                if !is_none_window(s.window) {
                    conn.send_request(&x::DestroyWindow { window: s.window });
                }
                drop(unsafe { Box::from_raw(s_ptr) });
            }
            m.live.clear();
            if m.colormap.resource_id() != 0 {
                conn.send_request(&x::FreeColormap { cmap: m.colormap });
            }
        }
        let _ = conn.flush();
    }
}

/// Clamp saved window geometry to the primary screen extent. Runs before
/// `init()` so it opens its own short-lived connection.
pub fn clamp_window_geometry(w: &mut i32, h: &mut i32) {
    let Ok((conn, _)) = xcb::Connection::connect(None) else {
        return;
    };
    let setup = conn.get_setup();
    let Some(root) = setup.roots().next() else {
        return;
    };
    let sw = root.width_in_pixels() as i32;
    let sh = root.height_in_pixels() as i32;
    if sw > 0 && *w > sw {
        *w = sw;
    }
    if sh > 0 && *h > sh {
        *h = sh;
    }
}
