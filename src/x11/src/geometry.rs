//! X11 geometry watcher thread: the executor for [`crate::overlay_fsm`].
//!
//! It blocks in `poll(-1)` with no timer. Running timer-free is only safe
//! because every change class emits an event on a window we watch:
//! `STRUCTURE_NOTIFY | PROPERTY_CHANGE` on the parent and its frame, plus
//! `STRUCTURE_NOTIFY` on each overlay so a WM clamp re-triggers a reconcile.

use std::os::fd::AsRawFd;
use std::sync::Arc;

use parking_lot::Mutex;
use x11rb::connection::Connection;
use x11rb::protocol::Event;
use x11rb::protocol::xfixes::{ConnectionExt as _, SelectionEventMask};
use x11rb::protocol::xproto::{
    AtomEnum, ChangeWindowAttributesAux, ClientMessageData, ClientMessageEvent, ConfigureWindowAux,
    ConnectionExt as _, EventMask, StackMode, Window,
};
use x11rb::rust_connection::RustConnection;

use jfn_playback::shutdown::jfn_shutdown_initiate;
use jfn_wake_event::WakeEvent;

use crate::input::x11_shutdown_waker;
use crate::overlay_fsm::{self, Effect, Geom};
use crate::x11_state::MUT;

pub struct Handle {
    join: Option<std::thread::JoinHandle<()>>,
}

impl Handle {
    pub fn join(&mut self) {
        if let Some(ev) = x11_shutdown_waker() {
            ev.signal();
        }
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
fn x11_geometry_resync_waker() -> Option<&'static WakeEvent> {
    use std::sync::OnceLock;
    static EV: OnceLock<Option<&'static WakeEvent>> = OnceLock::new();
    *EV.get_or_init(|| Some(Box::leak(Box::new(WakeEvent::new()?))))
}

pub fn request_resync() {
    if let Some(ev) = x11_geometry_resync_waker() {
        ev.signal();
    }
}

/// The mpv fullscreen callback. It only *triggers* a reconcile; the authority
/// for overlay fullscreen state is the parent's `_NET_WM_STATE`, read each pass.
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

fn watch_window(conn: &RustConnection, window: Window, mask: EventMask) {
    let aux = ChangeWindowAttributesAux::new().event_mask(mask);
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
fn query_geometry(conn: &RustConnection, window: Window, root: Window) -> Option<Geom> {
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

fn window_has_fullscreen(conn: &RustConnection, win: Window) -> bool {
    let (net_wm_state, fullscreen) = {
        let g = MUT.lock();
        let Some(m) = g.as_ref() else { return false };
        (m.atoms.net_wm_state, m.atoms.net_wm_state_fullscreen)
    };
    if let Ok(Ok(reply)) = conn
        .get_property(false, win, net_wm_state, AtomEnum::ATOM, 0, 64)
        .map(|c| c.reply())
        && let Some(mut vals) = reply.value32()
    {
        return vals.any(|a| a == fullscreen);
    }
    false
}

/// The geometric fallback covers WMs that fullscreen the parent before
/// publishing `_NET_WM_STATE`, so the overlays don't lag a frame behind.
fn read_parent_fullscreen(conn: &RustConnection, parent: Window, root: Window, geom: Geom) -> bool {
    if window_has_fullscreen(conn, parent) {
        return true;
    }
    if let Ok(Ok(rgeo)) = conn.get_geometry(root).map(|c| c.reply()) {
        return geom.2 >= rgeo.width as i32 && geom.3 >= rgeo.height as i32;
    }
    false
}

fn overlay_mapped(conn: &RustConnection, win: Window) -> Option<bool> {
    let r = conn.get_window_attributes(win).ok()?.reply().ok()?;
    Some(r.map_state != x11rb::protocol::xproto::MapState::UNMAPPED)
}

fn raise_to_top(conn: &RustConnection, win: Window) {
    let aux = ConfigureWindowAux::new().stack_mode(StackMode::ABOVE);
    let _ = conn.configure_window(win, &aux);
}

fn apply_effects(conn: &RustConnection, win: Window, effects: &[Effect]) {
    for e in effects {
        match *e {
            Effect::Poke { x, y } => {
                let aux = ConfigureWindowAux::new().x(x).y(y);
                let _ = conn.configure_window(win, &aux);
            }
            Effect::SetSize { w, h } => {
                let aux = ConfigureWindowAux::new().width(w as u32).height(h as u32);
                let _ = conn.configure_window(win, &aux);
            }
            Effect::SetOverrideRedirect(v) => {
                let _ = conn.unmap_window(win);
                let aux = ChangeWindowAttributesAux::new().override_redirect(u32::from(v));
                let _ = conn.change_window_attributes(win, &aux);
            }
            Effect::MapAndRaise => {
                let _ = conn.map_window(win);
                // The passive button grab may not survive the remap — re-grab.
                crate::input::grab_overlay_input(win);
                raise_to_top(conn, win);
            }
            Effect::Unmap => {
                let _ = conn.unmap_window(win);
            }
        }
    }
}

fn activate_parent(conn: &RustConnection, root: Window, parent: Window) {
    let net_active_window = {
        let g = MUT.lock();
        let Some(m) = g.as_ref() else { return };
        m.atoms.net_active_window
    };
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

/// Snapshot parent truth once, then derive every overlay's placement from that
/// one snapshot. Re-querying the parent per overlay would tear placement across
/// overlays when the parent changes mid-pass. `reassert_stack` is set for events
/// that can have restacked the parent (parent/frame changes, mpv activation) so
/// an unmanaged overlay is raised back over mpv; it is off for an overlay's own
/// events, which would otherwise feed a raise→notify→raise loop.
fn reconcile(
    conn: &RustConnection,
    parent: Window,
    root: Window,
    parent_mapped: bool,
    reassert_stack: bool,
) {
    let Some(parent_geom) = query_geometry(conn, parent, root) else {
        return;
    };
    let parent_fs = read_parent_fullscreen(conn, parent, root, parent_geom);

    let snaps = {
        let mut g = MUT.lock();
        let Some(m) = g.as_mut() else { return };
        crate::lifecycle::set_parent_geometry_locked(
            m,
            parent_geom.0,
            parent_geom.1,
            parent_geom.2,
            parent_geom.3,
        );
        m.parent_fullscreen = parent_fs;
        crate::lifecycle::snapshot_live_overlays_locked(m)
    };

    let mut updates = Vec::with_capacity(snaps.len());
    for snap in &snaps {
        // Round-trip first: proves the window exists server-side (created on the
        // control connection) before we select input on it, and gives the real
        // map state so the FSM can recover from a WM withdraw.
        let observed = query_geometry(conn, snap.window, root);
        if observed.is_some() {
            watch_window(conn, snap.window, EventMask::STRUCTURE_NOTIFY);
        }
        let observed_mapped = overlay_mapped(conn, snap.window);
        let mut state = snap.state;
        let inputs = overlay_fsm::Inputs {
            parent_geom,
            parent_fullscreen: parent_fs,
            want_visible: snap.visible && parent_mapped,
            owns_size: snap.send_size,
            observed,
            observed_mapped,
        };
        let effects = overlay_fsm::step(&mut state, &inputs);
        apply_effects(conn, snap.window, &effects);
        // The WM does not stack an unmanaged window, so when an external event
        // may have raised mpv, raise the overlay back over it.
        if reassert_stack && state.unmanaged && state.mapped {
            raise_to_top(conn, snap.window);
        }
        updates.push((snap.window, state));
    }
    let _ = conn.flush();

    let g = MUT.lock();
    if let Some(m) = g.as_ref() {
        for (win, state) in updates {
            crate::lifecycle::store_overlay_state_locked(m, win, state);
        }
    }
}

fn hide_overlays(conn: &RustConnection) {
    let snaps = {
        let g = MUT.lock();
        g.as_ref()
            .map(crate::lifecycle::snapshot_live_overlays_locked)
    };
    if let Some(snaps) = snaps {
        for s in &snaps {
            let _ = conn.unmap_window(s.window);
        }
        let _ = conn.flush();
    }
    jfn_playback::lifecycle::jfn_lifecycle_set_visible(false);
}

enum Trigger {
    Ignore,
    /// Parent/frame change — may have restacked mpv, so re-assert overlay stacking.
    External,
    /// An overlay's own event (e.g. the WM withdrawing it during a flip). Reconcile
    /// to recover, but do NOT re-raise — that would feed a raise→notify→raise loop.
    Overlay,
    ParentMap,
    ParentUnmap,
}

fn geometry_thread_body(conn: Arc<RustConnection>, parent: Window, root: Window) {
    // A frame move emits no client ConfigureNotify but does emit one on the
    // frame, so watch the frame too; PROPERTY_CHANGE delivers `_NET_WM_STATE`.
    let watch_mask = EventMask::STRUCTURE_NOTIFY | EventMask::PROPERTY_CHANGE;
    watch_window(&conn, parent, watch_mask);
    let mut frame = find_frame(&conn, parent, root);
    if frame != parent {
        watch_window(&conn, frame, watch_mask);
    }
    watch_compositor(&conn, root);
    let _ = conn.flush();

    let x11_fd = conn.stream().as_raw_fd();
    // -1 if waker allocation failed: poll ignores negative fds.
    let shutdown_fd = x11_shutdown_waker().map_or(-1, WakeEvent::fd);
    let resync_fd = x11_geometry_resync_waker().map_or(-1, WakeEvent::fd);

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

    let mut parent_mapped = true;
    reconcile(&conn, parent, root, parent_mapped, true);

    loop {
        let rc = unsafe { libc::poll(fds.as_mut_ptr(), 3, -1) };
        if rc < 0 {
            let err = std::io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            break;
        }

        if fds[1].revents & libc::POLLIN != 0 {
            hide_overlays(&conn);
            break;
        }
        if fds[0].revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0 {
            hide_overlays(&conn);
            break;
        }

        let mut wake = false;
        let mut reassert = false;
        let mut activate = false;
        if fds[2].revents & libc::POLLIN != 0 {
            if let Some(ev) = x11_geometry_resync_waker() {
                ev.drain();
            }
            // A new overlay or an mpv fullscreen toggle drove the resync — both
            // can change parent stacking.
            wake = true;
            reassert = true;
        }

        while let Ok(Some(ev)) = conn.poll_for_event() {
            match handle_event(&conn, parent, root, &mut frame, ev) {
                Trigger::Ignore => {}
                Trigger::External => {
                    wake = true;
                    reassert = true;
                }
                Trigger::Overlay => wake = true,
                Trigger::ParentMap => {
                    parent_mapped = true;
                    jfn_playback::lifecycle::jfn_lifecycle_set_visible(true);
                    wake = true;
                    reassert = true;
                    activate = true;
                }
                Trigger::ParentUnmap => {
                    parent_mapped = false;
                    jfn_playback::lifecycle::jfn_lifecycle_set_visible(false);
                    wake = true;
                }
            }
        }

        if wake {
            reconcile(&conn, parent, root, parent_mapped, reassert);
            if activate {
                // Re-mapping the transient overlays on top of the parent can
                // displace the WM's active window off mpv, stalling the
                // taskbar's minimize/activate toggle; re-assert it.
                activate_parent(&conn, root, parent);
            }
        }
    }
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
) -> Trigger {
    let is_parentish = |w: Window| w == parent || w == *frame;
    match ev {
        // Parent/frame move or restack → external. An overlay's own ConfigureNotify
        // (e.g. from our own raise, or the WM withdrawing it) → Overlay, so it
        // reconciles without re-raising.
        Event::ConfigureNotify(e) => {
            if is_parentish(e.window) {
                Trigger::External
            } else {
                Trigger::Overlay
            }
        }
        // A WM/pager that restacks via XCirculateSubwindows emits CirculateNotify
        // instead of ConfigureNotify.
        Event::CirculateNotify(e) => {
            if is_parentish(e.window) {
                Trigger::External
            } else {
                Trigger::Overlay
            }
        }
        // A fullscreen flip can land as a parent `_NET_WM_STATE` change with no
        // accompanying ConfigureNotify.
        Event::PropertyNotify(e) => {
            if e.window == parent {
                Trigger::External
            } else {
                Trigger::Ignore
            }
        }
        // The WM swaps the client into a different frame on fullscreen/maximize
        // toggles; re-resolve and re-watch the new frame.
        Event::ReparentNotify(e) => {
            if e.window == parent {
                let new_frame = find_frame(conn, parent, root);
                if new_frame != parent {
                    watch_window(
                        conn,
                        new_frame,
                        EventMask::STRUCTURE_NOTIFY | EventMask::PROPERTY_CHANGE,
                    );
                }
                *frame = new_frame;
                let _ = conn.flush();
                return Trigger::External;
            }
            Trigger::Ignore
        }
        Event::MapNotify(e) => {
            if e.window == parent {
                Trigger::ParentMap
            } else {
                Trigger::Ignore
            }
        }
        // An overlay's UnmapNotify is the WM withdrawing it during the flip;
        // reconcile so the FSM re-maps it (the withdraw happens only once).
        Event::UnmapNotify(e) => {
            if e.window == parent {
                Trigger::ParentUnmap
            } else {
                Trigger::Overlay
            }
        }
        // Only the client window's destruction is the teardown signal. A stale
        // frame we still hold STRUCTURE_NOTIFY on (never un-watched after a
        // reparent) emits DestroyNotify on fullscreen/maximize toggles — reacting
        // to it would quit the app mid-transition.
        Event::DestroyNotify(e) => {
            if e.window == parent {
                jfn_shutdown_initiate();
            }
            Trigger::Ignore
        }
        Event::ClientMessage(e) => {
            if e.window == parent && is_wm_delete(&e) {
                jfn_shutdown_initiate();
            }
            Trigger::Ignore
        }
        Event::XfixesSelectionNotify(e) => {
            if e.owner != x11rb::NONE {
                tracing::debug!(target: "Platform", "{}", crate::lifecycle::COMPOSITOR_DETECTED_MSG);
                Trigger::External
            } else {
                tracing::error!(target: "Platform", "{}", crate::lifecycle::COMPOSITOR_NOT_DETECTED_MSG);
                Trigger::Ignore
            }
        }
        _ => Trigger::Ignore,
    }
}
