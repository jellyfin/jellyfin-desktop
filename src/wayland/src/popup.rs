//! Context menu as a real `xdg_popup`.
//!
//! The menu `wl_surface` is **persistent** — created once and re-roled for each
//! menu. Destroying it would race the proxy's teardown of its role objects
//! across the two connection hops (a `wl_surface`-destroyed-before-its-role
//! protocol error); reusing it sidesteps that. Each show clears its buffer
//! first so the surface is role-free and buffer-free when the proxy re-roles it.
//!
//! `xdg_popup.grab` is only honored in response to the triggering input event,
//! with that event's serial — but CEF hands us the menu model later, via an
//! async callback. So [`arm`] creates+grabs the popup at the press (valid
//! serial) and leaves it unmapped (grab inert); [`show`] maps a 1×1 placeholder
//! (xdg_popup.reposition requires a mapped popup) then grows it to the menu.

use std::sync::atomic::{AtomicBool, Ordering};

use wayland_client::Proxy;
use wayland_client::protocol::wl_buffer::WlBuffer;
use wayland_client::protocol::wl_surface::WlSurface;
use wayland_protocols::wp::viewporter::client::wp_viewport::WpViewport;

use jfn_menu::MenuItem;
use jfn_menu::interaction_fsm::{self, MenuEffect, MenuEvent, MenuState as FsmState};
use jfn_menu::render::{self, Fonts, Layout};

use crate::wl_state::{WlState, create_shm_buffer, lock, try_state};

static MENU_ACTIVE: AtomicBool = AtomicBool::new(false);

#[derive(Default, Clone, Copy, PartialEq, Eq)]
enum Phase {
    #[default]
    Idle,
    AwaitPlaceholder,
    Placeholder,
    AwaitMenu,
    Shown,
}

#[derive(Default)]
pub struct MenuIo {
    fonts: Option<Fonts>,
    surface: Option<WlSurface>,
    viewport: Option<WpViewport>,
    buffer: Option<WlBuffer>,
    menu: Option<Menu>,
    phase: Phase,
}

struct Menu {
    items: Vec<MenuItem>,
    layout: Layout,
    fsm: FsmState,
    pw: i32,
    ph: i32,
    scale: f32,
    cb: Option<Box<dyn FnOnce(i32) + Send>>,
    mapped: bool,
    anchor: (i32, i32),
}

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

pub fn arm(x: i32, y: i32) {
    let mut st = lock();
    clear_menu_locked(&mut st);
    let surface_id = ensure_surface_locked(&mut st);
    st.menu_io.phase = Phase::AwaitPlaceholder;
    drop(st);

    jfn_wlproxy::jfn_wlproxy_set_popup_surface(surface_id);
    jfn_wlproxy::jfn_wlproxy_show_popup(x, y, 1, 1);
}

pub fn show(items: Vec<MenuItem>, x: i32, y: i32, cb: Box<dyn FnOnce(i32) + Send>) {
    let mut st = lock();

    let scale = crate::proxy::jfn_wl_get_cached_scale();
    let layout = {
        let fonts = st.menu_io.fonts.get_or_insert_with(Fonts::new);
        render::layout(fonts, &items, scale)
    };
    let pw = layout.width;
    let ph = layout.height;

    let phase = st.menu_io.phase;

    st.menu_io.menu = Some(Menu {
        items,
        layout,
        fsm: FsmState::default(),
        pw,
        ph,
        scale,
        cb: Some(cb),
        mapped: false,
        anchor: (x, y),
    });

    match phase {
        Phase::Placeholder => {
            let repos = begin_menu_locked(&mut st);
            drop(st);
            if let Some((x, y, lw, lh)) = repos {
                jfn_wlproxy::jfn_wlproxy_reposition_popup(x, y, lw, lh);
            }
        }
        // on_ready() starts the menu once the popup is configured.
        Phase::AwaitPlaceholder => {
            drop(st);
        }
        // Not armed by a triggering press: no grab popup exists, so create one
        // at full size now (its grab serial may be stale on this path).
        Phase::Idle => {
            let lw = logical_dim(pw, scale);
            let lh = logical_dim(ph, scale);
            let surface_id = ensure_surface_locked(&mut st);
            st.menu_io.phase = Phase::AwaitMenu;
            drop(st);
            jfn_wlproxy::jfn_wlproxy_set_popup_surface(surface_id);
            jfn_wlproxy::jfn_wlproxy_show_popup(x, y, lw, lh);
        }
        Phase::AwaitMenu | Phase::Shown => {
            drop(st);
        }
    }
}

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

fn begin_menu_locked(st: &mut WlState) -> Option<(i32, i32, i32, i32)> {
    paint_placeholder_locked(st);
    MENU_ACTIVE.store(true, Ordering::Release);
    let menu = st.menu_io.menu.as_ref()?;
    let (x, y) = menu.anchor;
    let lw = logical_dim(menu.pw, menu.scale);
    let lh = logical_dim(menu.ph, menu.scale);
    st.menu_io.phase = Phase::AwaitMenu;
    Some((x, y, lw, lh))
}

fn paint_placeholder_locked(st: &mut WlState) {
    let pixels = [0u8; 4]; // 1×1 transparent BGRA — maps the popup invisibly.
    let Some(buf) = create_shm_buffer(st, &pixels, 1, 1) else {
        return;
    };
    let Some(surface) = st.menu_io.surface.clone() else {
        return;
    };
    if let Some(vp) = st.menu_io.viewport.as_ref() {
        vp.set_source(0.0, 0.0, 1.0, 1.0);
        vp.set_destination(1, 1);
    }
    surface.attach(Some(&buf), 0, 0);
    surface.damage_buffer(0, 0, 1, 1);
    surface.commit();
    if let Some(old) = st.menu_io.buffer.replace(buf) {
        old.destroy();
    }
    st.flush();
}

pub(crate) fn on_ready() {
    let Some(state) = try_state() else { return };
    let mut st = state.lock();
    match st.menu_io.phase {
        Phase::AwaitPlaceholder => {
            if st.menu_io.menu.is_some() {
                let repos = begin_menu_locked(&mut st);
                drop(st);
                if let Some((x, y, lw, lh)) = repos {
                    jfn_wlproxy::jfn_wlproxy_reposition_popup(x, y, lw, lh);
                }
            } else {
                // Stay unmapped until the model arrives — the grab is inert while
                // unmapped, so mapping now would grab input with nothing to show.
                st.menu_io.phase = Phase::Placeholder;
            }
        }
        Phase::AwaitMenu => {
            paint_and_attach_locked(&mut st);
            if let Some(menu) = st.menu_io.menu.as_mut() {
                menu.mapped = true;
            }
            st.menu_io.phase = Phase::Shown;
        }
        _ => {}
    }
}

pub(crate) fn on_done() {
    let Some(state) = try_state() else { return };
    let mut st = state.lock();
    fire_locked(&mut st, -1);
    clear_menu_locked(&mut st);
}

pub fn active() -> bool {
    MENU_ACTIVE.load(Ordering::Acquire)
}

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

pub fn handle_motion(local_x: i32, local_y: i32) {
    let mut st = lock();
    let Some(menu) = st.menu_io.menu.as_ref().filter(|m| m.mapped) else {
        return;
    };
    let (px, py) = (
        (local_x as f32 * menu.scale) as i32,
        (local_y as f32 * menu.scale) as i32,
    );
    step_locked(&mut st, MenuEvent::Motion { x: px, y: py });
}

pub fn handle_button(local_x: i32, local_y: i32, pressed: bool) {
    if !pressed {
        return;
    }
    let mut st = lock();
    let Some(menu) = st.menu_io.menu.as_ref().filter(|m| m.mapped) else {
        return;
    };
    let (px, py) = (
        (local_x as f32 * menu.scale) as i32,
        (local_y as f32 * menu.scale) as i32,
    );
    step_locked(&mut st, MenuEvent::Press { x: px, y: py });
}

pub fn handle_outside_press() {
    let mut st = lock();
    if st.menu_io.menu.as_ref().filter(|m| m.mapped).is_none() {
        return;
    }
    step_locked(&mut st, MenuEvent::Dismiss);
}

pub fn handle_key(keysym: u32, pressed: bool) {
    if !pressed {
        return;
    }
    let mut st = lock();
    if st.menu_io.menu.as_ref().filter(|m| m.mapped).is_none() {
        return;
    }
    step_locked(&mut st, MenuEvent::Key(keysym));
}

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

fn clear_menu_locked(st: &mut WlState) {
    MENU_ACTIVE.store(false, Ordering::Release);
    st.menu_io.menu = None;
    st.menu_io.phase = Phase::Idle;
}
