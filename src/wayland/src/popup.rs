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

use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use wayland_client::Proxy;
use wayland_client::protocol::wl_buffer::WlBuffer;
use wayland_client::protocol::wl_surface::WlSurface;
use wayland_protocols::wp::viewporter::client::wp_viewport::WpViewport;

use jfn_menu::MenuItem;
use jfn_menu::interaction_fsm::{self, MenuEffect, MenuEvent, MenuState as FsmState};
use jfn_menu::render::{self, Fonts, Layout};

use crate::wl_state::{WlState, create_shm_buffer, lock, try_state};

static MENU_ACTIVE: AtomicBool = AtomicBool::new(false);
// True from menu map (grab activation) until teardown. Our menu's xdg_popup
// grab steals the Wayland keyboard, so the compositor sends the main surface a
// keyboard-leave; while engaged we must NOT forward that as focus-loss to CEF,
// or Blink closes the still-needed <select> popup out from under us.
static ENGAGED: AtomicBool = AtomicBool::new(false);
static NEXT_GENERATION: AtomicU32 = AtomicU32::new(1);

#[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
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
    generation: u32,
}

struct Menu {
    items: Vec<MenuItem>,
    layout: Layout,
    fsm: FsmState,
    pw: i32,
    /// Full content (buffer) height, physical px.
    ph: i32,
    /// Visible (window-clamped) height, physical px; the menu scrolls when
    /// smaller than `ph`.
    view_ph: i32,
    /// Scroll offset, physical px, `0..=ph - view_ph`.
    scroll: i32,
    scale: f32,
    cb: Option<Box<dyn FnOnce(i32) + Send>>,
    mapped: bool,
    anchor: (i32, i32),
}

impl Menu {
    fn row_height(&self) -> i32 {
        self.layout
            .rows
            .iter()
            .find(|r| !r.separator)
            .map_or(1, |r| r.h.max(1))
    }

    fn max_scroll(&self) -> i32 {
        (self.ph - self.view_ph).max(0)
    }

    fn scroll_active_into_view(&mut self) {
        if self.view_ph >= self.ph {
            return;
        }
        let Some(r) = self
            .layout
            .rows
            .iter()
            .find(|r| r.item as i32 == self.fsm.active)
        else {
            return;
        };
        if r.y < self.scroll {
            self.scroll = r.y;
        } else if r.y + r.h > self.scroll + self.view_ph {
            self.scroll = r.y + r.h - self.view_ph;
        }
        self.scroll = self.scroll.clamp(0, self.max_scroll());
    }
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
    let s = jfn_platform_abi::scale_or_one(f64::from(scale));
    jfn_platform_abi::PhysicalUnit::new(physical)
        .to_logical::<i32>(s)
        .0
        .max(1)
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
    let generation = next_generation();
    let mut st = lock();
    clear_menu_locked(&mut st);
    st.menu_io.generation = generation;
    let surface_id = ensure_surface_locked(&mut st);
    st.menu_io.phase = Phase::AwaitPlaceholder;
    drop(st);

    jfn_wlproxy::jfn_wlproxy_set_popup_surface(surface_id);
    jfn_wlproxy::jfn_wlproxy_show_popup(generation, x, y, 1, 1);
}

pub fn show(items: Vec<MenuItem>, x: i32, y: i32, cb: Box<dyn FnOnce(i32) + Send>) {
    show_highlighted(items, x, y, 0, -1, cb);
}

/// `width` is the desired logical menu width; `<= 0` falls back to
/// content-sized layout.
pub fn show_highlighted(
    items: Vec<MenuItem>,
    x: i32,
    y: i32,
    width: i32,
    initial: i32,
    cb: Box<dyn FnOnce(i32) + Send>,
) {
    let mut st = lock();

    let scale = jfn_platform_abi::scale_get_or_one() as f32;
    let layout = {
        let fonts = st.menu_io.fonts.get_or_insert_with(Fonts::new);
        let mut layout = render::layout(fonts, &items, scale);
        if width > 0 {
            layout.width = jfn_platform_abi::LogicalUnit::new(width)
                .to_physical::<i32>(f64::from(scale))
                .0
                .max(1);
        }
        layout
    };
    let pw = layout.width;
    let ph = layout.height;

    let phase = st.menu_io.phase;

    let mut menu = Menu {
        items,
        layout,
        fsm: FsmState { active: initial },
        pw,
        ph,
        view_ph: ph,
        scroll: 0,
        scale,
        cb: Some(cb),
        mapped: false,
        anchor: (x, y),
    };
    // Only select dropdowns (width > 0) clamp to the window bottom — context
    // menus stay full-height and rely on compositor flip/slide. Keep at least
    // one row when the anchor sits at the very bottom.
    let (_, win_ph) = crate::proxy::jfn_wl_window_size();
    if width > 0 && win_ph > 0 {
        let anchor_ph_y = (y as f32 * scale).round() as i32;
        let avail = win_ph - anchor_ph_y;
        menu.view_ph = ph.min(avail.max(menu.row_height()));
    }
    menu.scroll_active_into_view();
    let view_ph = menu.view_ph;
    st.menu_io.menu = Some(menu);

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
            let generation = next_generation();
            ENGAGED.store(true, Ordering::Release);
            st.menu_io.generation = generation;
            let lw = logical_dim(pw, scale);
            let lh = logical_dim(view_ph, scale);
            let surface_id = ensure_surface_locked(&mut st);
            st.menu_io.phase = Phase::AwaitMenu;
            drop(st);
            jfn_wlproxy::jfn_wlproxy_set_popup_surface(surface_id);
            jfn_wlproxy::jfn_wlproxy_show_popup(generation, x, y, lw, lh);
        }
        // Configure is still pending; on_ready() maps the replacement menu.
        Phase::AwaitMenu => {
            drop(st);
        }
        // No further configure will arrive, so the replacement menu must be
        // marked mapped and repainted here; left unmapped, the input handlers'
        // mapped filters go dead while MENU_ACTIVE swallows every click.
        Phase::Shown => {
            if let Some(menu) = st.menu_io.menu.as_mut() {
                menu.mapped = true;
            }
            paint_and_attach_locked(&mut st);
            let repos = st.menu_io.menu.as_ref().map(|m| {
                (
                    m.anchor.0,
                    m.anchor.1,
                    logical_dim(m.pw, m.scale),
                    logical_dim(m.view_ph, m.scale),
                )
            });
            drop(st);
            if let Some((x, y, lw, lh)) = repos {
                jfn_wlproxy::jfn_wlproxy_reposition_popup(x, y, lw, lh);
            }
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
    MENU_ACTIVE.store(true, Ordering::Release);
    // Before the map below: mapping activates the grab, and the grab-induced
    // keyboard-leave must not observe ENGAGED == false.
    ENGAGED.store(true, Ordering::Release);
    paint_placeholder_locked(st);
    let menu = st.menu_io.menu.as_ref()?;
    let (x, y) = menu.anchor;
    let lw = logical_dim(menu.pw, menu.scale);
    let lh = logical_dim(menu.view_ph, menu.scale);
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

pub(crate) fn on_ready(generation: u32) {
    let Some(state) = try_state() else { return };
    let mut st = state.lock();
    if st.menu_io.generation != generation {
        return;
    }
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

pub(crate) fn on_done(generation: u32) {
    let Some(state) = try_state() else { return };
    let mut st = state.lock();
    if st.menu_io.generation != generation {
        return;
    }
    fire_locked(&mut st, -1);
    clear_menu_locked(&mut st);
}

/// Tear down the menu without firing its selection callback. Used when CEF
/// hides its own `<select>` widget (e.g. focus loss) — the close originates
/// outside the FSM, so there is no pick to report.
pub fn hide() {
    let Some(state) = try_state() else { return };
    let mut st = state.lock();
    // An armed-but-menuless grab popup must survive: this hide can be the tail
    // of the previous cycle (or a Blink toggle-close) arriving after the next
    // press already armed; tearing the grab down forces the stale-serial Idle
    // path and kills every subsequent open.
    if st.menu_io.menu.is_none() {
        return;
    }
    clear_menu_locked(&mut st);
    drop(st);
    jfn_wlproxy::jfn_wlproxy_hide_popup();
}

pub fn active() -> bool {
    MENU_ACTIVE.load(Ordering::Acquire)
}

/// True from menu map until teardown — i.e. our menu's grab owns (or is about
/// to own) the seat. Used to suppress forwarding the grab-induced
/// keyboard-leave on the MAIN surface to CEF as focus-loss.
pub fn is_engaged() -> bool {
    ENGAGED.load(Ordering::Acquire)
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

/// True if `surface_id` is the persistent menu surface — unlike
/// [`surface_matches`], also when no menu is shown (the teardown
/// keyboard-leave arrives after the menu is already cleared).
pub fn is_menu_surface(surface_id: u32) -> bool {
    let Some(state) = try_state() else {
        return false;
    };
    let st = state.lock();
    st.menu_io
        .surface
        .as_ref()
        .is_some_and(|s| s.id().protocol_id() == surface_id)
}

pub fn handle_motion(local_x: i32, local_y: i32) {
    let mut st = lock();
    let Some(menu) = st.menu_io.menu.as_ref().filter(|m| m.mapped) else {
        return;
    };
    let p: jfn_platform_abi::PhysicalPosition =
        jfn_platform_abi::LogicalPosition::new(local_x, local_y)
            .to_physical(jfn_platform_abi::scale_or_one(f64::from(menu.scale)));
    let (px, py) = (p.x, p.y + menu.scroll);
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
    let p: jfn_platform_abi::PhysicalPosition =
        jfn_platform_abi::LogicalPosition::new(local_x, local_y)
            .to_physical(jfn_platform_abi::scale_or_one(f64::from(menu.scale)));
    let (px, py) = (p.x, p.y + menu.scroll);
    step_locked(&mut st, MenuEvent::Press { x: px, y: py });
}

/// Wheel scroll over the menu surface. `dy` uses the same convention as the
/// CEF scroll callback (±120 per detent, positive = wheel up).
pub fn handle_scroll(dy: i32) {
    let mut st = lock();
    let Some(menu) = st.menu_io.menu.as_mut().filter(|m| m.mapped) else {
        return;
    };
    if menu.view_ph >= menu.ph {
        return;
    }
    let step = (dy as f32 / 120.0 * menu.row_height() as f32).round() as i32;
    let new = (menu.scroll - step).clamp(0, menu.max_scroll());
    if new == menu.scroll {
        return;
    }
    menu.scroll = new;
    paint_and_attach_locked(&mut st);
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
    if matches!(ev, MenuEvent::Key(_)) {
        menu.scroll_active_into_view();
    }
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
    let (pw, ph, view_ph, scroll, scale, active) = (
        menu.pw,
        menu.ph,
        menu.view_ph,
        menu.scroll,
        menu.scale,
        menu.fsm.active,
    );
    let lw = logical_dim(pw, scale);
    let lh = logical_dim(view_ph, scale);
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
        vp.set_source(0.0, scroll as f64, pw as f64, view_ph as f64);
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
    ENGAGED.store(false, Ordering::Release);
    st.menu_io.menu = None;
    st.menu_io.phase = Phase::Idle;
    st.menu_io.generation = 0;
}

fn next_generation() -> u32 {
    NEXT_GENERATION
        .fetch_update(Ordering::AcqRel, Ordering::Acquire, |v| {
            Some(if v == u32::MAX { 1 } else { v + 1 })
        })
        .unwrap_or(1)
}
