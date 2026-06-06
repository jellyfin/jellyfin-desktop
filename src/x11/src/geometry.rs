//! X11 geometry watcher thread.

use std::os::fd::AsRawFd;
use std::sync::Arc;

use parking_lot::Mutex;
use x11rb::connection::Connection;
use x11rb::protocol::Event;
use x11rb::protocol::xfixes::{ConnectionExt as _, SelectionEventMask};
use x11rb::protocol::xproto::{
    AtomEnum, ChangeWindowAttributesAux, ClientMessageData, ClientMessageEvent, ConfigureWindowAux,
    ConnectionExt as _, EventMask, PropMode, Window,
};
use x11rb::rust_connection::RustConnection;
use x11rb::wrapper::ConnectionExt as _;

use jfn_playback::shutdown::jfn_shutdown_initiate;
use jfn_playback::wake_event::{jfn_wake_event_drain, jfn_wake_event_fd, jfn_wake_event_signal};

use crate::input::x11_shutdown_waker;
use crate::lifecycle::OverlaySnap;
use crate::x11_state::MUT;

/// Settle poll interval. Catches the final position-only frame move after
/// fullscreen/maximize exit, which can arrive with no ConfigureNotify of its
/// own.
const TICK_MS: i32 = 16;

pub struct Handle {
    join: Option<std::thread::JoinHandle<()>>,
}

impl Handle {
    pub fn join(&mut self) {
        unsafe { jfn_wake_event_signal(x11_shutdown_waker()) };
        if let Some(j) = self.join.take()
            && let Err(e) = j.join()
        {
            eprintln!("[x11] geometry thread panicked: {e:?}");
        }
    }
}

static G: Mutex<Option<Handle>> = Mutex::new(None);

/// A new CEF layer may be created after the parent has already reached its
/// final WM placement, so no further ConfigureNotify arrives to settle on;
/// signalling this waker re-mirrors the parent geometry onto the overlays.
fn x11_geometry_resync_waker() -> *const jfn_playback::WakeEvent {
    use std::sync::OnceLock;
    static EV: OnceLock<Option<&'static jfn_playback::WakeEvent>> = OnceLock::new();
    EV.get_or_init(|| {
        let raw = jfn_playback::WakeEvent::new()?;
        Some(Box::leak(Box::new(raw)))
    })
    .map_or(std::ptr::null(), |e| e as *const _)
}

pub fn request_resync() {
    unsafe { jfn_wake_event_signal(x11_geometry_resync_waker()) };
}

pub fn set_parent_fullscreen(fs: bool) {
    {
        let mut g = MUT.lock();
        let Some(m) = g.as_mut() else { return };
        m.parent_fullscreen = fs;
    }
    request_resync();
}

pub fn start(parent: u32, root: u32) {
    let conn = match RustConnection::connect(None) {
        Ok((conn, _)) => Arc::new(conn),
        Err(e) => {
            eprintln!("[x11] geometry watcher failed to connect: {e:?}");
            return;
        }
    };
    let join = match std::thread::Builder::new()
        .name("jfn-x11-geometry".into())
        .spawn(move || geometry_thread_body(conn, parent, root))
    {
        Ok(j) => j,
        Err(e) => {
            eprintln!("[x11] failed to spawn geometry thread: {e}");
            return;
        }
    };
    *G.lock() = Some(Handle { join: Some(join) });
}

pub fn cleanup() {
    let mut g = G.lock();
    if let Some(h) = g.as_mut() {
        h.join();
    }
    *g = None;
}

fn find_frame(conn: &RustConnection, mut w: Window, root: Window) -> Window {
    loop {
        let Ok(cookie) = conn.query_tree(w) else {
            return w;
        };
        let Ok(reply) = cookie.reply() else {
            return w;
        };
        let parent = reply.parent;
        if parent == 0 || parent == root {
            return w;
        }
        w = parent;
    }
}

fn watch_structure(conn: &RustConnection, window: Window) {
    let aux = ChangeWindowAttributesAux::new().event_mask(EventMask::STRUCTURE_NOTIFY);
    let _ = conn.change_window_attributes(window, &aux);
}

/// Subscribe to ownership changes of the `_NET_WM_CM_S{n}` selection so we learn
/// when a compositing manager starts or stops mid-session.
fn watch_compositor(conn: &RustConnection, root: Window) {
    let screen_num = {
        let g = MUT.lock();
        let Some(m) = g.as_ref() else { return };
        m.screen_num
    };
    if !matches!(
        conn.xfixes_query_version(5, 0).map(|c| c.reply()),
        Ok(Ok(_))
    ) {
        return;
    }
    let Ok(Ok(atom)) = conn
        .intern_atom(false, crate::lifecycle::cm_atom_name(screen_num).as_bytes())
        .map(|c| c.reply())
    else {
        return;
    };
    let mask = SelectionEventMask::SET_SELECTION_OWNER
        | SelectionEventMask::SELECTION_WINDOW_DESTROY
        | SelectionEventMask::SELECTION_CLIENT_CLOSE;
    let _ = conn.xfixes_select_selection_input(root, atom.atom, mask);
}

/// Query a window's absolute screen position + size. Returns None on protocol failure.
fn query_geometry(
    conn: &RustConnection,
    window: Window,
    root: Window,
) -> Option<(i32, i32, i32, i32)> {
    let geo = conn.get_geometry(window).ok()?.reply().ok()?;
    let trans = conn
        .translate_coordinates(window, root, 0, 0)
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

fn reposition_overlays(
    conn: &RustConnection,
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
        let mut aux = ConfigureWindowAux::new().x(px).y(py);
        if s.send_size {
            aux = aux.width(pw as u32).height(ph as u32);
        }
        let _ = conn.configure_window(s.window, &aux);
    }
    let _ = conn.flush();
}

/// Per EWMH a mapped window's `_NET_WM_STATE` changes via a root client message;
/// an unmapped one's via a property rewrite, read by the WM on its next map.
fn apply_fullscreen_state(conn: &RustConnection, root: Window, snaps: &[OverlaySnap], fs: bool) {
    let (net_wm_state, skip_taskbar, skip_pager, fullscreen) = {
        let g = MUT.lock();
        let Some(m) = g.as_ref() else { return };
        (
            m.atoms.net_wm_state,
            m.atoms.net_wm_state_skip_taskbar,
            m.atoms.net_wm_state_skip_pager,
            m.atoms.net_wm_state_fullscreen,
        )
    };
    let action = u32::from(fs);
    let mut prop = vec![skip_taskbar, skip_pager];
    if fs {
        prop.push(fullscreen);
    }
    for s in snaps {
        if s.visible {
            let ev = ClientMessageEvent::new(
                32,
                s.window,
                net_wm_state,
                ClientMessageData::from([action, fullscreen, 0, 1, 0]),
            );
            let _ = conn.send_event(
                false,
                root,
                EventMask::SUBSTRUCTURE_NOTIFY | EventMask::SUBSTRUCTURE_REDIRECT,
                ev,
            );
        } else {
            let _ = conn.change_property32(
                PropMode::REPLACE,
                s.window,
                net_wm_state,
                u32::from(AtomEnum::ATOM),
                &prop,
            );
        }
    }
    let _ = conn.flush();
}

fn map_overlays(conn: &RustConnection, snaps: &[OverlaySnap]) {
    for s in snaps.iter().filter(|s| s.visible) {
        let _ = conn.map_window(s.window);
    }
    let _ = conn.flush();
}

fn unmap_overlays(conn: &RustConnection, snaps: &[OverlaySnap]) {
    for s in snaps {
        let _ = conn.unmap_window(s.window);
    }
    let _ = conn.flush();
}

fn activate_parent(conn: &RustConnection, root: Window, parent: Window, net_active_window: u32) {
    let ev = ClientMessageEvent::new(
        32,
        parent,
        net_active_window,
        ClientMessageData::from([2, 0, 0, 0, 0]),
    );
    let _ = conn.send_event(
        false,
        root,
        EventMask::SUBSTRUCTURE_NOTIFY | EventMask::SUBSTRUCTURE_REDIRECT,
        ev,
    );
    let _ = conn.flush();
}

fn geometry_thread_body(conn: Arc<RustConnection>, parent: Window, root: Window) {
    // A frame move emits no ConfigureNotify on the client, so without also
    // watching the frame the overlays never learn the window's new position
    // after fullscreen/maximize exit.
    watch_structure(&conn, parent);
    let mut frame = find_frame(&conn, parent, root);
    if frame != parent {
        watch_structure(&conn, frame);
    }
    watch_compositor(&conn, root);
    let _ = conn.flush();

    let x11_fd = conn.stream().as_raw_fd();
    let shutdown_fd = unsafe { jfn_wake_event_fd(x11_shutdown_waker()) };
    let resync_fd = unsafe { jfn_wake_event_fd(x11_geometry_resync_waker()) };

    let mut fds: [libc::pollfd; 3] = [
        libc::pollfd {
            fd: x11_fd,
            events: libc::POLLIN,
            revents: 0,
        },
        libc::pollfd {
            fd: shutdown_fd,
            events: libc::POLLIN,
            revents: 0,
        },
        libc::pollfd {
            fd: resync_fd,
            events: libc::POLLIN,
            revents: 0,
        },
    ];

    let mut settling = false;
    let mut applied_fs = false;

    loop {
        let timeout = if settling { TICK_MS } else { -1 };
        let rc = unsafe { libc::poll(fds.as_mut_ptr(), 3, timeout) };
        if rc < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            break;
        }

        if fds[1].revents & libc::POLLIN != 0 {
            set_visibility(&conn, parent, root, false);
            break;
        }
        if fds[0].revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
            set_visibility(&conn, parent, root, false);
            break;
        }

        if fds[2].revents & libc::POLLIN != 0 {
            unsafe { jfn_wake_event_drain(x11_geometry_resync_waker()) };
            resync_and_track_fullscreen(&conn, parent, root, &mut applied_fs);
            if !settling {
                settling = true;
                tracing::debug!(target: "x11::settle", "settle started (layer created)");
            }
        }

        let mut geometry_changed = false;
        while let Ok(Some(ev)) = conn.poll_for_event() {
            geometry_changed |= handle_event(&conn, parent, root, &mut frame, ev);
        }
        if geometry_changed {
            resync_and_track_fullscreen(&conn, parent, root, &mut applied_fs);
            if !settling {
                settling = true;
                tracing::debug!(target: "x11::settle", "settle started (geometry changed)");
            }
        }

        if settling && rc == 0 {
            let (matched, mpv, samples) = settle_tick(&conn, parent, root);
            for (win, overlay) in &samples {
                tracing::debug!(
                    target: "x11::settle",
                    "compare overlay=0x{win:x} overlay_geom={overlay:?} mpv={mpv:?} match={}",
                    *overlay == Some(mpv)
                );
            }
            if matched {
                settling = false;
                tracing::debug!(target: "x11::settle", "settled: all overlays match mpv={mpv:?}");
            }
        }
    }
}

type Geom = (i32, i32, i32, i32);
fn settle_tick(
    conn: &RustConnection,
    parent: Window,
    root: Window,
) -> (bool, Geom, Vec<(u32, Option<Geom>)>) {
    let geo = query_geometry(conn, parent, root);

    let (mpv, snaps) = {
        let mut g = MUT.lock();
        let Some(m) = g.as_mut() else {
            return (true, (0, 0, 0, 0), Vec::new());
        };
        if let Some((px, py, pw, ph)) = geo {
            crate::lifecycle::set_parent_geometry_locked(m, px, py, pw, ph);
        }
        let mpv = (m.parent_x, m.parent_y, m.pw, m.ph);
        (mpv, crate::lifecycle::snapshot_live_overlays_locked(m))
    };

    reposition_overlays(conn, mpv.0, mpv.1, mpv.2, mpv.3, &snaps);

    let mut all_match = true;
    let mut samples = Vec::new();
    for s in &snaps {
        if !s.visible {
            continue;
        }
        let overlay = query_geometry(conn, s.window, root);
        all_match &= overlay == Some(mpv);
        samples.push((s.window, overlay));
    }

    (all_match, mpv, samples)
}

fn resync_overlays(
    conn: &RustConnection,
    parent: Window,
    root: Window,
) -> Option<(Vec<OverlaySnap>, bool)> {
    let (px, py, pw, ph) = query_geometry(conn, parent, root)?;
    let (snaps, fullscreen) = {
        let mut g = MUT.lock();
        let m = g.as_mut()?;
        crate::lifecycle::set_parent_geometry_locked(m, px, py, pw, ph);
        (
            crate::lifecycle::snapshot_live_overlays_locked(m),
            m.parent_fullscreen,
        )
    };
    reposition_overlays(conn, px, py, pw, ph, &snaps);
    Some((snaps, fullscreen))
}

/// Resync fires on every ConfigureNotify; only re-emit the state on a real flip.
fn resync_and_track_fullscreen(
    conn: &RustConnection,
    parent: Window,
    root: Window,
    applied_fs: &mut bool,
) {
    if let Some((snaps, fs)) = resync_overlays(conn, parent, root)
        && fs != *applied_fs
    {
        apply_fullscreen_state(conn, root, &snaps, fs);
        *applied_fs = fs;
    }
}

fn set_visibility(conn: &RustConnection, parent: Window, root: Window, visible: bool) {
    if visible {
        if let Some((px, py, pw, ph)) = query_geometry(conn, parent, root) {
            let snapshot = {
                let mut g = MUT.lock();
                g.as_mut().map(|m| {
                    crate::lifecycle::set_parent_geometry_locked(m, px, py, pw, ph);
                    (
                        crate::lifecycle::snapshot_live_overlays_locked(m),
                        m.atoms.net_active_window,
                    )
                })
            };
            if let Some((snaps, net_active_window)) = snapshot {
                reposition_overlays(conn, px, py, pw, ph, &snaps);
                map_overlays(conn, &snaps);
                // Re-mapping the transient overlays on top of the parent can
                // displace the WM's active window off mpv, stalling the
                // taskbar's minimize/activate toggle; re-assert it.
                activate_parent(conn, root, parent, net_active_window);
            }
        }
    } else {
        let snaps = {
            let mut g = MUT.lock();
            g.as_mut()
                .map(|m| crate::lifecycle::snapshot_live_overlays_locked(m))
        };
        if let Some(snaps) = snaps {
            unmap_overlays(conn, &snaps);
        }
    }
    jfn_playback::lifecycle::jfn_lifecycle_set_visible(visible);
}

fn is_wm_delete(e: &ClientMessageEvent) -> bool {
    let g = MUT.lock();
    let Some(m) = g.as_ref() else {
        return false;
    };
    e.type_ == m.atoms.wm_protocols && e.data.as_data32()[0] == m.atoms.wm_delete_window
}

fn handle_event(
    conn: &RustConnection,
    parent: Window,
    root: Window,
    frame: &mut Window,
    ev: Event,
) -> bool {
    match ev {
        Event::ConfigureNotify(e) => e.window == parent || e.window == *frame,
        // A WM/pager that restacks via XCirculateSubwindows emits CirculateNotify
        // instead of ConfigureNotify; treat it as a geometry change so the
        // settle re-asserts overlay stacking.
        Event::CirculateNotify(e) => e.window == parent || e.window == *frame,
        // The WM swaps the client into a different frame on fullscreen/maximize
        // toggles; re-resolve and re-watch the new frame.
        Event::ReparentNotify(e) => {
            if e.window == parent {
                let new_frame = find_frame(conn, parent, root);
                if new_frame != parent {
                    watch_structure(conn, new_frame);
                }
                *frame = new_frame;
                let _ = conn.flush();
                return true;
            }
            false
        }
        Event::MapNotify(e) => {
            if e.window == parent {
                set_visibility(conn, parent, root, true);
            }
            false
        }
        Event::UnmapNotify(e) => {
            if e.window == parent {
                set_visibility(conn, parent, root, false);
            }
            false
        }
        // Only the client window's destruction is the teardown signal. A stale
        // frame we still hold STRUCTURE_NOTIFY on (never un-watched after a
        // reparent) emits DestroyNotify on fullscreen/maximize toggles — reacting
        // to it would quit the app mid-transition.
        Event::DestroyNotify(e) => {
            if e.window == parent {
                jfn_shutdown_initiate();
            }
            false
        }
        Event::ClientMessage(e) => {
            if e.window == parent && is_wm_delete(&e) {
                jfn_shutdown_initiate();
            }
            false
        }
        Event::XfixesSelectionNotify(e) => {
            if e.owner != x11rb::NONE {
                tracing::debug!(target: "Platform", "{}", crate::lifecycle::COMPOSITOR_DETECTED_MSG);
                request_resync();
            } else {
                tracing::error!(target: "Platform", "{}", crate::lifecycle::COMPOSITOR_NOT_DETECTED_MSG);
            }
            false
        }
        _ => false,
    }
}
