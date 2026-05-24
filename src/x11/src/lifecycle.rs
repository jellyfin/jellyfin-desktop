//! X11 init/cleanup/clamp and helpers for atom interning, ARGB visual
//! discovery, parent geometry queries, and overlay repositioning.

use std::sync::Arc;

use xcb::{Xid, XidNew, x};

use crate::shm::shm_free;
use crate::x11_state::{Atoms, CONN, MUT, Mutable, is_none_gc, is_none_window};

use jfn_mpv::api::jfn_mpv_get_property_int;

/// Find a 32-bit TrueColor visual.
fn find_argb_visual(screen: &x::Screen, depth_out: &mut u8) -> Option<x::Visualid> {
    for depth in screen.allowed_depths() {
        if depth.depth() != 32 {
            continue;
        }
        for vis in depth.visuals() {
            if vis.class() == x::VisualClass::TrueColor {
                *depth_out = 32;
                return Some(vis.visual_id());
            }
        }
    }
    None
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

/// Reposition every live surface to match mpv's parent window. Called
/// from the input thread on ConfigureNotify. Caller must hold `MUT`.
pub fn sync_overlay_positions_locked(conn: &xcb::Connection, m: &mut Mutable) {
    let Some((px, py, pw, ph)) = query_parent_geometry(conn, m.parent, m.root) else {
        return;
    };
    m.parent_x = px;
    m.parent_y = py;

    for &s_ptr in &m.live {
        if s_ptr.is_null() {
            continue;
        }
        let s = unsafe { &*s_ptr };
        if is_none_window(s.window) || !s.visible {
            continue;
        }
        conn.send_request(&x::ConfigureWindow {
            window: s.window,
            value_list: &[
                x::ConfigWindow::X(px),
                x::ConfigWindow::Y(py),
                x::ConfigWindow::Width(pw as u32),
                x::ConfigWindow::Height(ph as u32),
            ],
        });
    }
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
            eprintln!("[x11] failed to connect: {:?}", e);
            return false;
        }
    };

    let setup = conn.get_setup();
    let screen = match setup.roots().nth(screen_num as usize) {
        Some(s) => s,
        None => {
            eprintln!("[x11] no screen at index {}", screen_num);
            return false;
        }
    };
    let root = screen.root();

    let mut argb_depth: u8 = 0;
    let argb_visual = match find_argb_visual(screen, &mut argb_depth) {
        Some(v) => v,
        None => {
            eprintln!("[x11] no 32-bit ARGB visual found");
            return false;
        }
    };

    let colormap: x::Colormap = conn.generate_id();
    conn.send_request(&x::CreateColormap {
        alloc: x::ColormapAlloc::None,
        mid: colormap,
        window: root,
        visual: argb_visual,
    });

    let atoms = Atoms {
        net_wm_opacity: intern_atom(&conn, b"_NET_WM_WINDOW_OPACITY"),
        net_wm_window_type: intern_atom(&conn, b"_NET_WM_WINDOW_TYPE"),
        net_wm_window_type_notification: intern_atom(&conn, b"_NET_WM_WINDOW_TYPE_NOTIFICATION"),
        net_wm_state: intern_atom(&conn, b"_NET_WM_STATE"),
        net_wm_state_above: intern_atom(&conn, b"_NET_WM_STATE_ABOVE"),
        net_wm_state_skip_taskbar: intern_atom(&conn, b"_NET_WM_STATE_SKIP_TASKBAR"),
        net_wm_state_skip_pager: intern_atom(&conn, b"_NET_WM_STATE_SKIP_PAGER"),
        wm_protocols: intern_atom(&conn, b"WM_PROTOCOLS"),
        wm_delete_window: intern_atom(&conn, b"WM_DELETE_WINDOW"),
    };

    // Verify the MIT-SHM extension is present.
    let shm_cookie = conn.send_request(&xcb::shm::QueryVersion {});
    if conn.wait_for_reply(shm_cookie).is_err() {
        eprintln!("[x11] MIT-SHM extension not available");
        return false;
    }

    let (parent_x, parent_y, pw, ph) =
        query_parent_geometry(&conn, parent, root).unwrap_or((0, 0, 1, 1));

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
        });
    }

    let conn = Arc::new(conn);
    if CONN.set(conn.clone()).is_err() {
        eprintln!("[x11] connection already initialized");
        return false;
    }

    // Spawn input thread on mpv's parent window. Configure callback runs
    // `sync_overlay_positions_locked`; shutdown callback hides surfaces.
    crate::input_lifecycle::start(conn.clone(), parent);

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

    jfn_idle_inhibit_linux::cleanup();
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
                for buf in &mut s.bufs {
                    shm_free(buf, Some(&conn));
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
