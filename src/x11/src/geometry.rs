//! X11 geometry watcher thread.
//!
//! Owns its own `xcb::Connection` and selects only `STRUCTURE_NOTIFY` on
//! mpv's top-level window. Geometry repositioning needs a GetGeometry /
//! TranslateCoordinates round-trip per resize; keeping it on a dedicated
//! connection means that round-trip never stalls keyboard/pointer intake on
//! the input thread.

use std::os::fd::AsRawFd;
use std::sync::Arc;

use parking_lot::Mutex;
use xcb::{Xid, x};

use jfn_playback::shutdown::jfn_shutdown_initiate;
use jfn_playback::wake_event::{jfn_wake_event_drain, jfn_wake_event_fd, jfn_wake_event_signal};

use crate::input::x11_shutdown_waker;
use crate::lifecycle::query_parent_geometry;
use crate::x11_state::MUT;

/// After a geometry change, re-query and reposition every TICK_MS until every
/// visible overlay's actual on-server geometry matches mpv's window geometry,
/// then stop. Catches the final position-only frame move after
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

/// Process-global waker the geometry thread polls so other threads can ask it
/// to re-mirror the parent geometry onto the overlays. Signalled when a new CEF
/// layer is created, since the parent may already have reached its final WM
/// placement before the layer existed (no further ConfigureNotify to settle on).
/// Leaked like [`x11_shutdown_waker`]; not part of the shutdown fan-out.
fn x11_geometry_resync_waker() -> *const jfn_playback::WakeEvent {
    use std::sync::OnceLock;
    static EV: OnceLock<&'static jfn_playback::WakeEvent> = OnceLock::new();
    *EV.get_or_init(|| {
        let raw = jfn_playback::WakeEvent::new().expect("x11 geometry resync waker allocation");
        Box::leak(Box::new(raw))
    }) as *const _
}

/// Ask the geometry thread to re-sync overlays to the current parent geometry.
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

/// Walk up from `w` to the window whose parent is `root` — the WM frame that
/// actually moves when the window is dragged or restored. Falls back to `w`
/// itself when there is no reparenting WM (or the window is already a root
/// child, e.g. reparented to root while fullscreen).
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
    // Watch the client window (size + reparent + map state) and its WM frame
    // (screen position). A frame move emits no real ConfigureNotify on the
    // client, so without watching the frame the overlays never learn the
    // window's new position after fullscreen/maximize exit.
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

    // While settling, poll wakes every TICK_MS to re-query and reposition
    // until every visible overlay matches mpv's window geometry. Idle
    // otherwise (infinite timeout, zero cost).
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

        // A new layer was created: mirror the current geometry onto it now, then
        // settle to converge over any WM placement still in flight.
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
        // Coalesced: one resync per wake regardless of how many ConfigureNotify
        // were drained — they all read the same current geometry anyway.
        if geometry_changed {
            resync_overlays(&conn, parent, root);
            if !settling {
                settling = true;
                tracing::debug!(target: "x11::settle", "settle started (geometry changed)");
            }
        }

        if settling && rc == 0 {
            // Tick: reposition overlays to mpv, then read back each overlay's
            // actual geometry and compare it to mpv's. Stop once all match.
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

/// One settle tick: reposition overlays to mpv's geometry, then query each
/// visible overlay's actual on-server geometry. Returns whether all match
/// mpv, mpv's geometry, and per-overlay `(window_id, actual_geometry)` samples
/// for logging.
///
/// The blocking parent query and the per-overlay readbacks run lock-free; `MUT`
/// is held only to apply the freshly queried geometry and snapshot the overlay
/// window ids, so a settle tick never stalls the CEF paint path on the lock.
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

    // Settled: re-assert overlay stacking above the parent. A WM can drop the
    // transient overlays behind the video on a fullscreen transition, leaving
    // CEF invisible; this makes above-parent stacking deterministic.
    if all_match {
        crate::lifecycle::restack_overlays_above(conn, parent, &snaps);
        tracing::debug!(target: "x11::settle", "restacked overlays above parent");
    }
    (all_match, mpv, samples)
}

/// Mirror the current parent geometry onto the overlays. `MUT` is held only to
/// record the geometry and copy the overlay list; the `ConfigureWindow` sends +
/// flush run lock-free so this never stalls the CEF paint path on the lock.
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
        // Query parent geometry lock-free before taking MUT, and issue the
        // reposition/map sends after releasing it, so a show never stalls the
        // CEF paint path on the lock.
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

/// Classify an event: returns true when window geometry changed. The caller does
/// a single coalesced `resync_overlays` per wake afterward (so a burst of
/// ConfigureNotify doesn't trigger one redundant resync each) and opens a settle
/// window to converge on a position that may arrive without a further event.
fn handle_event(
    conn: &xcb::Connection,
    parent: x::Window,
    root: x::Window,
    frame: &mut x::Window,
    ev: xcb::Event,
) -> bool {
    use xcb::Event;
    match ev {
        // Either the client (size) or its WM frame (screen position) moved.
        Event::X(x::Event::ConfigureNotify(e)) => {
            let w = e.window().resource_id();
            w == parent.resource_id() || w == frame.resource_id()
        }
        // A WM/pager that restacks via XCirculateSubwindows emits CirculateNotify
        // instead of ConfigureNotify; treat it as a geometry change so the
        // settle re-asserts overlay stacking above the parent.
        Event::X(x::Event::CirculateNotify(e)) => {
            let w = e.window().resource_id();
            w == parent.resource_id() || w == frame.resource_id()
        }
        // The WM swapped the client into a different frame (common on
        // fullscreen/maximize toggles). Re-resolve and re-watch the new frame
        // here (stateful, per-event); the caller's post-drain resync mirrors the
        // settled geometry.
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
        // Forward map-state changes to the manager FSM so CEF can drop GPU
        // compositing resources while iconified.
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
