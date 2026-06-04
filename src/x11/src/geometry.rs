//! X11 geometry watcher thread.

use std::os::fd::AsRawFd;
use std::sync::Arc;

use parking_lot::Mutex;
use xcb::{Xid, x};

use jfn_playback::shutdown::jfn_shutdown_initiate;
use jfn_playback::wake_event::{jfn_wake_event_drain, jfn_wake_event_fd, jfn_wake_event_signal};

use crate::input::x11_shutdown_waker;
use crate::lifecycle::query_parent_geometry;
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

pub fn start(parent: x::Window, root: x::Window) {
    let conn = match xcb::Connection::connect(None) {
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

fn find_frame(conn: &xcb::Connection, mut w: x::Window, root: x::Window) -> x::Window {
    loop {
        let Ok(reply) = conn.wait_for_reply(conn.send_request(&x::QueryTree { window: w })) else {
            return w;
        };
        let parent = reply.parent();
        if parent.resource_id() == 0 || parent.resource_id() == root.resource_id() {
            return w;
        }
        w = parent;
    }
}

fn watch_structure(conn: &xcb::Connection, window: x::Window) {
    conn.send_request(&x::ChangeWindowAttributes {
        window,
        value_list: &[x::Cw::EventMask(x::EventMask::STRUCTURE_NOTIFY)],
    });
}

fn geometry_thread_body(conn: Arc<xcb::Connection>, parent: x::Window, root: x::Window) {
    // A frame move emits no ConfigureNotify on the client, so without also
    // watching the frame the overlays never learn the window's new position
    // after fullscreen/maximize exit.
    watch_structure(&conn, parent);
    let mut frame = find_frame(&conn, parent, root);
    if frame.resource_id() != parent.resource_id() {
        watch_structure(&conn, frame);
    }
    let _ = conn.flush();

    let xcb_fd = conn.as_raw_fd();
    let shutdown_fd = unsafe { jfn_wake_event_fd(x11_shutdown_waker()) };
    let resync_fd = unsafe { jfn_wake_event_fd(x11_geometry_resync_waker()) };

    let mut fds: [libc::pollfd; 3] = [
        libc::pollfd {
            fd: xcb_fd,
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
    conn: &xcb::Connection,
    parent: x::Window,
    root: x::Window,
) -> (bool, Geom, Vec<(u32, Option<Geom>)>) {
    let geo = query_parent_geometry(conn, parent, root);

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

    crate::lifecycle::reposition_overlays(conn, mpv.0, mpv.1, mpv.2, mpv.3, &snaps);

    let mut all_match = true;
    let mut samples = Vec::new();
    for s in &snaps {
        if !s.visible {
            continue;
        }
        let overlay = query_parent_geometry(conn, s.window, root);
        all_match &= overlay == Some(mpv);
        samples.push((s.window.resource_id(), overlay));
    }

    // A WM can drop the transient overlays behind the video on a fullscreen
    // transition, leaving CEF invisible; re-assert above-parent stacking.
    if all_match {
        crate::lifecycle::restack_overlays_above(conn, parent, &snaps);
        tracing::debug!(target: "x11::settle", "restacked overlays above parent");
    }
    (all_match, mpv, samples)
}

fn resync_overlays(conn: &xcb::Connection, parent: x::Window, root: x::Window) {
    let Some((px, py, pw, ph)) = query_parent_geometry(conn, parent, root) else {
        return;
    };
    let snaps = {
        let mut g = MUT.lock();
        let Some(m) = g.as_mut() else { return };
        crate::lifecycle::set_parent_geometry_locked(m, px, py, pw, ph);
        crate::lifecycle::snapshot_live_overlays_locked(m)
    };
    crate::lifecycle::reposition_overlays(conn, px, py, pw, ph, &snaps);
}

fn set_visibility(conn: &xcb::Connection, parent: x::Window, root: x::Window, visible: bool) {
    if visible {
        if let Some((px, py, pw, ph)) = query_parent_geometry(conn, parent, root) {
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
                crate::lifecycle::reposition_overlays(conn, px, py, pw, ph, &snaps);
                crate::lifecycle::map_overlays(conn, &snaps);
                // Restore (un-minimize) re-maps without necessarily emitting a
                // ConfigureNotify, so settle-restack may not fire — re-assert
                // stacking here too.
                crate::lifecycle::restack_overlays_above(conn, parent, &snaps);
                // Re-mapping the transient overlays on top of the parent can
                // displace the WM's active window off mpv, stalling the
                // taskbar's minimize/activate toggle; re-assert it.
                crate::lifecycle::activate_parent(conn, root, parent, net_active_window);
            }
        }
    } else {
        let snaps = {
            let mut g = MUT.lock();
            g.as_mut()
                .map(|m| crate::lifecycle::snapshot_live_overlays_locked(m))
        };
        if let Some(snaps) = snaps {
            crate::lifecycle::unmap_overlays(conn, &snaps);
        }
    }
    jfn_playback::lifecycle::jfn_lifecycle_set_visible(visible);
}

fn handle_event(
    conn: &xcb::Connection,
    parent: x::Window,
    root: x::Window,
    frame: &mut x::Window,
    ev: xcb::Event,
) -> bool {
    use xcb::Event;
    match ev {
        Event::X(x::Event::ConfigureNotify(e)) => {
            let w = e.window().resource_id();
            w == parent.resource_id() || w == frame.resource_id()
        }
        // A WM/pager that restacks via XCirculateSubwindows emits CirculateNotify
        // instead of ConfigureNotify; treat it as a geometry change so the
        // settle re-asserts overlay stacking.
        Event::X(x::Event::CirculateNotify(e)) => {
            let w = e.window().resource_id();
            w == parent.resource_id() || w == frame.resource_id()
        }
        // The WM swaps the client into a different frame on fullscreen/maximize
        // toggles; re-resolve and re-watch the new frame.
        Event::X(x::Event::ReparentNotify(e)) => {
            if e.window().resource_id() == parent.resource_id() {
                let new_frame = find_frame(conn, parent, root);
                if new_frame.resource_id() != parent.resource_id() {
                    watch_structure(conn, new_frame);
                }
                *frame = new_frame;
                let _ = conn.flush();
                return true;
            }
            false
        }
        Event::X(x::Event::MapNotify(e)) => {
            if e.window().resource_id() == parent.resource_id() {
                set_visibility(conn, parent, root, true);
            }
            false
        }
        Event::X(x::Event::UnmapNotify(e)) => {
            if e.window().resource_id() == parent.resource_id() {
                set_visibility(conn, parent, root, false);
            }
            false
        }
        Event::X(x::Event::DestroyNotify(_)) => {
            jfn_shutdown_initiate();
            false
        }
        Event::X(x::Event::ClientMessage(_)) => {
            jfn_shutdown_initiate();
            false
        }
        _ => false,
    }
}
