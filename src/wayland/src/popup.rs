//! Context menu as a real `xdg_popup`.
//!
//! The host renders the menu (via [`jfn_menu`]) into a persistent `wl_surface`
//! and registers it with the proxy; the proxy re-roles that surface as an
//! `xdg_popup` of the root toplevel, grabs it, and drives its lifecycle. The
//! compositor then constrains the menu on-screen and delivers input to it even
//! where it overhangs the window edge — the thing the old subsurface menu (with
//! its empty input region + coordinate matching on the main surface) could not
//! do.
//!
//! Split of ownership:
//!   * host (here): the menu surface, its buffer, [`jfn_menu`] layout/paint/FSM,
//!     the selection callback, and routing real seat events into the FSM.
//!   * proxy: positioner, xdg_surface, xdg_popup, grab, and the ready/popup_done
//!     lifecycle events bridged back via FFI callbacks.
//!
//! The menu `wl_surface` is **persistent** — created once and re-roled for each
//! menu. Destroying it would race the proxy's teardown of its role objects
//! across the two connection hops (a `wl_surface`-destroyed-before-its-role
//! protocol error); reusing it sidesteps that entirely. Each show clears its
//! buffer first so the surface is role-free and buffer-free when the proxy
//! re-roles it.
//!
//! All state lives in [`MenuIo`] inside `WlState`, so every entry point (the
//! CEF UI thread via `context_menu_show`, the input thread, and the proxy
//! callback thread) serialises on the single `WlState` lock.

use std::sync::atomic::{AtomicBool, Ordering};

use wayland_client::Proxy;
use wayland_client::protocol::wl_buffer::WlBuffer;
use wayland_client::protocol::wl_surface::WlSurface;
use wayland_protocols::wp::viewporter::client::wp_viewport::WpViewport;

use jfn_menu::MenuItem;
use jfn_menu::interaction_fsm::{self, MenuEffect, MenuEvent, MenuState as FsmState};
use jfn_menu::render::{self, Fonts, Layout};

use crate::wl_state::{WlState, create_shm_buffer, lock, try_state};

/// Cheap gate so the input thread can skip menu routing without locking.
static MENU_ACTIVE: AtomicBool = AtomicBool::new(false);

/// Persistent menu IO, owned by `WlState`. Holds the shared font cache, the
/// persistent menu surface, and the live menu model (when shown).
#[derive(Default)]
pub struct MenuIo {
    fonts: Option<Fonts>,
    surface: Option<WlSurface>,
    viewport: Option<WpViewport>,
    buffer: Option<WlBuffer>,
    menu: Option<Menu>,
}

struct Menu {
    items: Vec<MenuItem>,
    layout: Layout,
    fsm: FsmState,
    /// Physical (rendered) and logical (compositor) size.
    pw: i32,
    ph: i32,
    scale: f32,
    cb: Option<Box<dyn FnOnce(i32) + Send>>,
    mapped: bool,
}

// =====================================================================
// Paint helpers (BGRA premultiplied for wl_shm Argb8888)
// =====================================================================

fn to_bgra(rgba: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; rgba.len()];
    let mut i = 0;
    while i + 3 < rgba.len() {
        out[i] = rgba[i + 2];
        out[i + 1] = rgba[i + 1];
        out[i + 2] = rgba[i];
        out[i + 3] = rgba[i + 3];
        i += 4;
    }
    out
}

fn logical_dim(physical: i32, scale: f32) -> i32 {
    if scale > 0.0 {
        ((physical as f32 / scale).round() as i32).max(1)
    } else {
        physical.max(1)
    }
}

fn paint_bgra(
    fonts: &mut Fonts,
    layout: &Layout,
    items: &[MenuItem],
    active: i32,
) -> Option<Vec<u8>> {
    let pm = render::paint(fonts, layout, items, active)?;
    Some(to_bgra(pm.data()))
}

// =====================================================================
// Show / lifecycle
// =====================================================================

/// Build + show a context menu anchored at logical (x, y) in the window. The
/// (persistent) menu surface is cleared and registered now; the proxy re-roles
/// it as a popup and the buffer is attached once [`on_ready`] fires.
pub fn show(items: Vec<MenuItem>, x: i32, y: i32, cb: Box<dyn FnOnce(i32) + Send>) {
    let mut st = lock();

    // Replace any in-flight menu deterministically (no selection reported).
    if st.menu_io.menu.is_some() {
        clear_menu_locked(&mut st);
        jfn_wlproxy::jfn_wlproxy_hide_popup();
    }

    let scale = crate::proxy::jfn_wl_get_cached_scale();
    let layout = {
        let fonts = st.menu_io.fonts.get_or_insert_with(Fonts::new);
        render::layout(fonts, &items, scale)
    };
    let pw = layout.width;
    let ph = layout.height;
    let lw = logical_dim(pw, scale);
    let lh = logical_dim(ph, scale);

    // Create the persistent surface on first use; otherwise reuse it. Clear any
    // prior buffer so it is role-free AND buffer-free before the proxy re-roles
    // it (xdg forbids a buffer committed before the first configure).
    let surface_id = ensure_surface_locked(&mut st);

    st.menu_io.menu = Some(Menu {
        items,
        layout,
        fsm: FsmState::default(),
        pw,
        ph,
        scale,
        cb: Some(cb),
        mapped: false,
    });
    MENU_ACTIVE.store(true, Ordering::Release);
    drop(st);

    // Hand the surface to the proxy and ask it to create the popup.
    jfn_wlproxy::jfn_wlproxy_set_popup_surface(surface_id);
    jfn_wlproxy::jfn_wlproxy_show_popup(x, y, lw, lh);
}

/// Create (once) and clear the persistent menu surface; returns its protocol id.
fn ensure_surface_locked(st: &mut WlState) -> u32 {
    if st.menu_io.surface.is_none() {
        let surface = st.compositor.create_surface(&st.qh, ());
        let viewport = st
            .viewporter
            .as_ref()
            .map(|v| v.get_viewport(&surface, &st.qh, ()));
        st.menu_io.surface = Some(surface);
        st.menu_io.viewport = viewport;
    }
    // Detach any prior buffer so re-roling sees a buffer-free surface.
    if let Some(old) = st.menu_io.buffer.take() {
        old.destroy();
    }
    if let Some(surface) = st.menu_io.surface.as_ref() {
        surface.attach(None, 0, 0);
        surface.commit();
    }
    st.flush();
    st.menu_io
        .surface
        .as_ref()
        .map(|s| s.id().protocol_id())
        .unwrap_or(0)
}

/// Proxy callback: the menu's xdg_surface was configured. Attach the rendered
/// buffer and commit so the popup maps. Runs on the proxy client thread.
pub(crate) fn on_ready() {
    let Some(state) = try_state() else { return };
    let mut st = state.lock();
    match st.menu_io.menu.as_ref() {
        Some(m) if !m.mapped => {}
        _ => return,
    }
    paint_and_attach_locked(&mut st);
    if let Some(menu) = st.menu_io.menu.as_mut() {
        menu.mapped = true;
    }
}

/// Proxy callback: the compositor dismissed the popup (click-outside / Escape).
/// Report no selection and drop the menu model. The proxy has already torn down
/// its role objects. Runs on the proxy client thread.
pub(crate) fn on_done() {
    let Some(state) = try_state() else { return };
    let mut st = state.lock();
    fire_locked(&mut st, -1);
    clear_menu_locked(&mut st);
}

// =====================================================================
// Input routing (called from input.rs while a menu is active)
// =====================================================================

/// Cheap active check for the input thread's fast path.
pub fn active() -> bool {
    MENU_ACTIVE.load(Ordering::Acquire)
}

/// True if `surface_id` is the menu surface's protocol id.
pub fn surface_matches(surface_id: u32) -> bool {
    let Some(state) = try_state() else {
        return false;
    };
    let st = state.lock();
    st.menu_io.menu.is_some()
        && st
            .menu_io
            .surface
            .as_ref()
            .is_some_and(|s| s.id().protocol_id() == surface_id)
}

/// Pointer motion in menu-surface-local logical coords.
pub fn handle_motion(local_x: i32, local_y: i32) {
    let mut st = lock();
    let Some(menu) = st.menu_io.menu.as_ref() else {
        return;
    };
    let (px, py) = (
        (local_x as f32 * menu.scale) as i32,
        (local_y as f32 * menu.scale) as i32,
    );
    step_locked(&mut st, MenuEvent::Motion { x: px, y: py });
}

/// Pointer button in menu-surface-local logical coords.
pub fn handle_button(local_x: i32, local_y: i32, pressed: bool) {
    if !pressed {
        return;
    }
    let mut st = lock();
    let Some(menu) = st.menu_io.menu.as_ref() else {
        return;
    };
    let (px, py) = (
        (local_x as f32 * menu.scale) as i32,
        (local_y as f32 * menu.scale) as i32,
    );
    step_locked(&mut st, MenuEvent::Press { x: px, y: py });
}

/// Keyboard key (xkb keysym) while the menu is focused.
pub fn handle_key(keysym: u32, pressed: bool) {
    if !pressed {
        return;
    }
    let mut st = lock();
    if st.menu_io.menu.is_none() {
        return;
    }
    step_locked(&mut st, MenuEvent::Key(keysym));
}

// =====================================================================
// Internals (all run under the WlState lock)
// =====================================================================

fn step_locked(st: &mut WlState, ev: MenuEvent) {
    let Some(menu) = st.menu_io.menu.as_mut() else {
        return;
    };
    let effects = interaction_fsm::step(&mut menu.fsm, &ev, &menu.layout, &menu.items);
    for e in effects {
        match e {
            MenuEffect::Redraw => {
                if st.menu_io.menu.as_ref().is_some_and(|m| m.mapped) {
                    paint_and_attach_locked(st);
                }
            }
            MenuEffect::Close(id) => {
                fire_locked(st, id);
                clear_menu_locked(st);
                jfn_wlproxy::jfn_wlproxy_hide_popup();
                return;
            }
        }
    }
}

/// Paint the current menu at its active row and attach the buffer to the
/// persistent surface (maps it on first call, updates highlight after).
fn paint_and_attach_locked(st: &mut WlState) {
    let Some(menu) = st.menu_io.menu.as_ref() else {
        return;
    };
    let (pw, ph, scale, active) = (menu.pw, menu.ph, menu.scale, menu.fsm.active);
    let lw = logical_dim(pw, scale);
    let lh = logical_dim(ph, scale);
    let pixels = {
        let layout = menu.layout.clone();
        let items = menu.items.clone();
        let fonts = st.menu_io.fonts.get_or_insert_with(Fonts::new);
        paint_bgra(fonts, &layout, &items, active)
    };
    let Some(pixels) = pixels else { return };
    let Some(buf) = create_shm_buffer(st, &pixels, pw, ph) else {
        return;
    };
    let Some(surface) = st.menu_io.surface.clone() else {
        return;
    };
    if let Some(vp) = st.menu_io.viewport.as_ref() {
        vp.set_source(0.0, 0.0, pw as f64, ph as f64);
        vp.set_destination(lw, lh);
    }
    surface.attach(Some(&buf), 0, 0);
    surface.damage_buffer(0, 0, pw, ph);
    surface.commit();
    if let Some(old) = st.menu_io.buffer.replace(buf) {
        old.destroy();
    }
    st.flush();
}

fn fire_locked(st: &mut WlState, id: i32) {
    if let Some(menu) = st.menu_io.menu.as_mut()
        && let Some(cb) = menu.cb.take()
    {
        cb(id);
    }
}

/// Drop the per-show menu model. The persistent surface is kept (cleared on the
/// next show); only the proxy tears down the popup's role objects.
fn clear_menu_locked(st: &mut WlState) {
    MENU_ACTIVE.store(false, Ordering::Release);
    st.menu_io.menu = None;
}
