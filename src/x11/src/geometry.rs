//! X11 geometry watcher thread.

use std::os::fd::AsRawFd;
use std::sync::Arc;

use parking_lot::Mutex;
use x11rb::connection::Connection;
use x11rb::protocol::Event;
use x11rb::protocol::xproto::{
    ChangeWindowAttributesAux, ClientMessageData, ClientMessageEvent, ConfigureWindowAux,
    ConnectionExt as _, EventMask, StackMode, Window,
};
use x11rb::rust_connection::RustConnection;

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
    static EV: OnceLock<&'static jfn_playback::WakeEvent> = OnceLock::new();
    *EV.get_or_init(|| {
        let raw = jfn_playback::WakeEvent::new().expect("x11 geometry resync waker allocation");
        Box::leak(Box::new(raw))
    }) as *const _
}

pub fn request_resync() {
    unsafe { jfn_wake_event_signal(x11_geometry_resync_waker()) };
}

pub fn start(parent: u32, root: u32) {
    let conn = match RustConnection::connect(None) {
        Ok((conn, _)) => Arc::new(conn),
        Err(e) => {
            eprintln!("[x11] geometry watcher failed to connect: {e:?}");
            return;
        }
    };
    let join = std::thread::Builder::new()
        .name("jfn-x11-geometry".into())
        .spawn(move || geometry_thread_body(conn, parent, root))
        .expect("spawn x11 geometry thread");
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

fn restack_overlays_above(conn: &RustConnection, parent: Window, snaps: &[OverlaySnap]) {
    let mut prev = parent;
    for s in snaps.iter().filter(|s| s.visible) {
        let win = s.window;
        if win == prev {
            continue;
        }
        let aux = ConfigureWindowAux::new()
            .sibling(prev)
            .stack_mode(StackMode::ABOVE);
        let _ = conn.configure_window(win, &aux);
        prev = win;
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
            resync_overlays(&conn, parent, root);
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
            resync_overlays(&conn, parent, root);
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

    // A WM can drop the transient overlays behind the video on a fullscreen
    // transition, leaving CEF invisible; re-assert above-parent stacking.
    if all_match {
        restack_overlays_above(conn, parent, &snaps);
        tracing::debug!(target: "x11::settle", "restacked overlays above parent");
    }
    (all_match, mpv, samples)
}

fn resync_overlays(conn: &RustConnection, parent: Window, root: Window) {
    let Some((px, py, pw, ph)) = query_geometry(conn, parent, root) else {
        return;
    };
    let snaps = {
        let mut g = MUT.lock();
        let Some(m) = g.as_mut() else { return };
        crate::lifecycle::set_parent_geometry_locked(m, px, py, pw, ph);
        crate::lifecycle::snapshot_live_overlays_locked(m)
    };
    reposition_overlays(conn, px, py, pw, ph, &snaps);
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
                // Restore (un-minimize) re-maps without necessarily emitting a
                // ConfigureNotify, so settle-restack may not fire — re-assert
                // stacking here too.
                restack_overlays_above(conn, parent, &snaps);
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
        Event::DestroyNotify(_) => {
            jfn_shutdown_initiate();
            false
        }
        Event::ClientMessage(_) => {
            jfn_shutdown_initiate();
            false
        }
        _ => false,
    }
}
