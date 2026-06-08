//! Pure model of the Wayland surface tree: layer stacking order and the
//! context menu — all mutated only through [`reduce`], which returns
//! [`Effect`]s for the IO layer ([`sink`]) to apply.
//!
//! Determinism by construction: every transition that changes on-screen state
//! emits the explicit effect that makes it land (notably a [`Effect::CommitParent`]
//! after any stacking change — Wayland subsurface placement is parent-double-
//! buffered, so without this the z-order silently never applies). The reducer is
//! the single writer; it holds no Wayland handles and no threads, so the whole
//! surface-tree logic is unit-testable headlessly by asserting the effect stream.

pub mod sink;

use std::sync::atomic::{AtomicBool, Ordering};

use jfn_menu::MenuItem;
use jfn_menu::interaction_fsm::{self, MenuEffect, MenuEvent};
use jfn_menu::render::Layout;

use crate::wl_state::{WlState, lock};
use sink::SceneSink;

/// Opaque layer identity. In production this is a `*mut PlatformSurface`
/// address; the reducer only ever compares ids.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)]
pub struct LayerId(pub usize);

/// What a layer's subsurface stacks directly above.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Above {
    Parent,
    Layer(LayerId),
}

struct MenuRec {
    parent: LayerId,
    origin: (i32, i32),
    scale: f32,
    layout: Layout,
    items: Vec<MenuItem>,
    state: interaction_fsm::MenuState,
}

#[derive(Default)]
pub struct Scene {
    order: Vec<LayerId>,
    applied: Vec<LayerId>,
    menu: Option<MenuRec>,
}

pub enum SceneEvent {
    LayerAdded(LayerId),
    LayerRemoved(LayerId),
    Restack(Vec<LayerId>),
    MenuShow {
        parent: LayerId,
        origin: (i32, i32),
        scale: f32,
        layout: Layout,
        items: Vec<MenuItem>,
    },
    /// Parent-relative logical pointer position.
    MenuPointerMotion {
        x: i32,
        y: i32,
    },
    MenuPointerPress {
        x: i32,
        y: i32,
    },
    MenuKey(u32),
    MenuDismiss,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Effect {
    PlaceAbove { layer: LayerId, above: Above },
    CommitParent,
    MenuCreate,
    MenuRedraw,
    MenuDestroy,
    Fire(i32),
}

impl Scene {
    fn has(&self, id: LayerId) -> bool {
        self.order.contains(&id)
    }

    pub fn menu_active(&self) -> bool {
        self.menu.is_some()
    }

    pub(crate) fn top_layer(&self) -> Option<LayerId> {
        self.order.last().copied()
    }

    /// Re-emit the full bottom-to-top `place_above` chain plus the single
    /// trailing `CommitParent` that makes it land. No-op when the order has not
    /// changed since it was last applied.
    fn restack_effects(&mut self) -> Vec<Effect> {
        if self.order == self.applied {
            return Vec::new();
        }
        let mut out = Vec::with_capacity(self.order.len() + 1);
        let mut prev: Option<LayerId> = None;
        for &id in &self.order {
            let above = match prev {
                None => Above::Parent,
                Some(p) => Above::Layer(p),
            };
            out.push(Effect::PlaceAbove { layer: id, above });
            prev = Some(id);
        }
        if !out.is_empty() {
            out.push(Effect::CommitParent);
        }
        self.applied = self.order.clone();
        out
    }

    /// Dismiss the menu if `removed` is its parent. Returns teardown effects.
    fn orphan_if_parent(&mut self, removed: LayerId) -> Vec<Effect> {
        if self.menu.as_ref().is_some_and(|m| m.parent == removed) {
            self.menu = None;
            vec![Effect::MenuDestroy, Effect::Fire(-1)]
        } else {
            Vec::new()
        }
    }

    fn menu_interaction(&mut self, ev: MenuEvent) -> Vec<Effect> {
        let Some(menu) = self.menu.as_mut() else {
            return Vec::new();
        };
        let effects = interaction_fsm::step(&mut menu.state, &ev, &menu.layout, &menu.items);
        let mut out = Vec::new();
        for e in effects {
            match e {
                MenuEffect::Redraw => out.push(Effect::MenuRedraw),
                MenuEffect::Close(id) => {
                    self.menu = None;
                    out.push(Effect::MenuDestroy);
                    out.push(Effect::Fire(id));
                    break;
                }
            }
        }
        out
    }

    /// Parent-relative logical → menu-local physical (the space `Layout` is in).
    fn to_local(menu: &MenuRec, x: i32, y: i32) -> (i32, i32) {
        let lx = ((x - menu.origin.0) as f32 * menu.scale).round() as i32;
        let ly = ((y - menu.origin.1) as f32 * menu.scale).round() as i32;
        (lx, ly)
    }
}

pub fn reduce(scene: &mut Scene, ev: SceneEvent) -> Vec<Effect> {
    match ev {
        SceneEvent::LayerAdded(id) => {
            if !scene.has(id) {
                scene.order.push(id);
            }
            scene.restack_effects()
        }
        SceneEvent::LayerRemoved(id) => {
            let mut out = scene.orphan_if_parent(id);
            scene.order.retain(|&l| l != id);
            out.extend(scene.restack_effects());
            out
        }
        SceneEvent::Restack(order) => {
            // Keep only currently-known layers, preserving the requested order.
            let known: Vec<LayerId> = order.into_iter().filter(|id| scene.has(*id)).collect();
            scene.order = known;
            let mut out = Vec::new();
            if let Some(m) = scene.menu.as_ref()
                && !scene.order.contains(&m.parent)
            {
                out.extend(scene.orphan_if_parent(m.parent));
            }
            out.extend(scene.restack_effects());
            out
        }
        SceneEvent::MenuShow {
            parent,
            origin,
            scale,
            layout,
            items,
        } => {
            let mut out = Vec::new();
            // Replace any existing menu deterministically.
            if scene.menu.take().is_some() {
                out.push(Effect::MenuDestroy);
                out.push(Effect::Fire(-1));
            }
            if !scene.has(parent) {
                // No valid parent to anchor to — refuse and dismiss.
                out.push(Effect::Fire(-1));
                return out;
            }
            scene.menu = Some(MenuRec {
                parent,
                origin,
                scale,
                layout,
                items,
                state: interaction_fsm::MenuState::default(),
            });
            out.push(Effect::MenuCreate);
            out
        }
        SceneEvent::MenuPointerMotion { x, y } => {
            let Some(menu) = scene.menu.as_ref() else {
                return Vec::new();
            };
            let (lx, ly) = Scene::to_local(menu, x, y);
            scene.menu_interaction(MenuEvent::Motion { x: lx, y: ly })
        }
        SceneEvent::MenuPointerPress { x, y } => {
            let Some(menu) = scene.menu.as_ref() else {
                return Vec::new();
            };
            let (lx, ly) = Scene::to_local(menu, x, y);
            scene.menu_interaction(MenuEvent::Press { x: lx, y: ly })
        }
        SceneEvent::MenuKey(sym) => scene.menu_interaction(MenuEvent::Key(sym)),
        SceneEvent::MenuDismiss => {
            if scene.menu.take().is_some() {
                vec![Effect::MenuDestroy, Effect::Fire(-1)]
            } else {
                Vec::new()
            }
        }
    }
}

// =====================================================================
// IO driver + thread entry points
// =====================================================================

/// Cheap gate the input thread checks before locking. Mirrors
/// `Scene::menu_active`, refreshed after every dispatch.
static MENU_ACTIVE: AtomicBool = AtomicBool::new(false);

/// Reduce `ev` and apply its effects via the real Wayland sink. Caller holds
/// the wl_state lock. The single writer for the surface tree.
pub(crate) fn dispatch(st: &mut WlState, ev: SceneEvent) {
    let effects = reduce(&mut st.scene, ev);
    {
        let mut s = sink::WlSink::new(st);
        for e in &effects {
            s.apply(e);
        }
    }
    MENU_ACTIVE.store(st.scene.menu_active(), Ordering::Release);
    st.flush();
}

fn dispatch_locked(ev: SceneEvent) {
    let mut g = lock();
    dispatch(&mut g, ev);
}

pub(crate) fn active() -> bool {
    MENU_ACTIVE.load(Ordering::Acquire)
}

/// Open a native context menu anchored to the topmost layer. Called off the
/// input thread (CEF UI). `cb` fires with the chosen command id, or `-1`.
pub(crate) fn show_menu(items: Vec<MenuItem>, x: i32, y: i32, cb: Box<dyn FnOnce(i32) + Send>) {
    let mut g = lock();
    if g.scene.menu_active() {
        drop(g);
        cb(-1);
        return;
    }
    let Some(parent) = g.scene.top_layer() else {
        drop(g);
        cb(-1);
        return;
    };
    let scale = crate::proxy::jfn_wl_get_cached_scale();
    let layout = {
        let fonts = g.menu_io.fonts_mut();
        jfn_menu::render::layout(fonts, &items, scale)
    };
    g.menu_io.arm(cb);
    dispatch(
        &mut g,
        SceneEvent::MenuShow {
            parent,
            origin: (x, y),
            scale,
            layout,
            items,
        },
    );
}

/// Pointer motion (parent-relative logical). Returns true if consumed.
pub(crate) fn handle_motion(x: i32, y: i32) -> bool {
    if !active() {
        return false;
    }
    dispatch_locked(SceneEvent::MenuPointerMotion { x, y });
    true
}

/// Pointer button. Press drives selection/dismiss; all clicks are consumed
/// while a menu is open.
pub(crate) fn handle_button(x: i32, y: i32, pressed: bool) -> bool {
    if !active() {
        return false;
    }
    if pressed {
        dispatch_locked(SceneEvent::MenuPointerPress { x, y });
    }
    true
}

/// Keyboard key. Returns true if consumed.
pub(crate) fn handle_key(keysym: u32, pressed: bool) -> bool {
    if !active() {
        return false;
    }
    if pressed {
        dispatch_locked(SceneEvent::MenuKey(keysym));
    }
    true
}

/// Dismiss with no selection (e.g. keyboard focus loss — the no-grab fallback).
pub(crate) fn dismiss() {
    if !active() {
        return;
    }
    dispatch_locked(SceneEvent::MenuDismiss);
}

#[cfg(test)]
mod tests {
    use super::*;
    use jfn_menu::render::Row;

    const MAIN: LayerId = LayerId(1);
    const ABOUT: LayerId = LayerId(2);

    fn add(scene: &mut Scene, id: LayerId) -> Vec<Effect> {
        reduce(scene, SceneEvent::LayerAdded(id))
    }

    #[test]
    fn add_layer_stacks_above_parent_and_commits() {
        let mut s = Scene::default();
        let e = add(&mut s, MAIN);
        assert_eq!(
            e,
            vec![
                Effect::PlaceAbove {
                    layer: MAIN,
                    above: Above::Parent
                },
                Effect::CommitParent,
            ]
        );
    }

    #[test]
    fn second_layer_stacks_above_first_then_commits() {
        let mut s = Scene::default();
        add(&mut s, MAIN);
        let e = add(&mut s, ABOUT);
        assert_eq!(
            e,
            vec![
                Effect::PlaceAbove {
                    layer: MAIN,
                    above: Above::Parent
                },
                Effect::PlaceAbove {
                    layer: ABOUT,
                    above: Above::Layer(MAIN)
                },
                Effect::CommitParent,
            ]
        );
    }

    /// Regression lock: any event that changes the order MUST end in exactly one
    /// CommitParent — this is the invariant whose absence made About invisible.
    #[test]
    fn every_order_change_ends_in_single_commit_parent() {
        let mut s = Scene::default();
        for ev in [
            SceneEvent::LayerAdded(MAIN),
            SceneEvent::LayerAdded(ABOUT),
            SceneEvent::Restack(vec![ABOUT, MAIN]),
            SceneEvent::LayerRemoved(ABOUT),
        ] {
            let e = reduce(&mut s, ev);
            let commits = e.iter().filter(|x| **x == Effect::CommitParent).count();
            assert_eq!(commits, 1, "expected exactly one CommitParent, got {e:?}");
            assert_eq!(e.last(), Some(&Effect::CommitParent));
        }
    }

    #[test]
    fn restack_to_unchanged_order_is_noop() {
        let mut s = Scene::default();
        add(&mut s, MAIN);
        add(&mut s, ABOUT);
        let e = reduce(&mut s, SceneEvent::Restack(vec![MAIN, ABOUT]));
        assert_eq!(e, vec![]);
    }

    #[test]
    fn restack_reorders_and_commits() {
        let mut s = Scene::default();
        add(&mut s, MAIN);
        add(&mut s, ABOUT);
        let e = reduce(&mut s, SceneEvent::Restack(vec![ABOUT, MAIN]));
        assert_eq!(
            e,
            vec![
                Effect::PlaceAbove {
                    layer: ABOUT,
                    above: Above::Parent
                },
                Effect::PlaceAbove {
                    layer: MAIN,
                    above: Above::Layer(ABOUT)
                },
                Effect::CommitParent,
            ]
        );
    }

    fn menu_layout() -> Layout {
        // Two selectable rows at y=4..14 and y=14..24; width 100, height 28.
        let rows = vec![
            Row {
                item: 0,
                y: 4,
                h: 10,
                separator: false,
                enabled: true,
            },
            Row {
                item: 1,
                y: 14,
                h: 10,
                separator: false,
                enabled: true,
            },
        ];
        Layout::for_test(100, 28, rows, vec![0, 1])
    }

    fn menu_items() -> Vec<MenuItem> {
        vec![
            MenuItem {
                id: 10,
                label: "a".into(),
                enabled: true,
                separator: false,
            },
            MenuItem {
                id: 20,
                label: "b".into(),
                enabled: true,
                separator: false,
            },
        ]
    }

    fn show_menu(s: &mut Scene, parent: LayerId, scale: f32) -> Vec<Effect> {
        reduce(
            s,
            SceneEvent::MenuShow {
                parent,
                origin: (200, 100),
                scale,
                layout: menu_layout(),
                items: menu_items(),
            },
        )
    }

    #[test]
    fn menu_show_on_valid_parent_creates() {
        let mut s = Scene::default();
        add(&mut s, ABOUT);
        assert_eq!(show_menu(&mut s, ABOUT, 1.0), vec![Effect::MenuCreate]);
        assert!(s.menu_active());
    }

    #[test]
    fn menu_show_on_missing_parent_fires_dismiss() {
        let mut s = Scene::default();
        assert_eq!(show_menu(&mut s, ABOUT, 1.0), vec![Effect::Fire(-1)]);
        assert!(!s.menu_active());
    }

    #[test]
    fn removing_menu_parent_tears_menu_down_then_restacks() {
        let mut s = Scene::default();
        add(&mut s, MAIN);
        add(&mut s, ABOUT);
        show_menu(&mut s, ABOUT, 1.0);
        let e = reduce(&mut s, SceneEvent::LayerRemoved(ABOUT));
        assert_eq!(e[0], Effect::MenuDestroy);
        assert_eq!(e[1], Effect::Fire(-1));
        assert_eq!(e.last(), Some(&Effect::CommitParent));
        assert!(!s.menu_active());
    }

    #[test]
    fn restack_excluding_menu_parent_orphans_menu() {
        let mut s = Scene::default();
        add(&mut s, MAIN);
        add(&mut s, ABOUT);
        show_menu(&mut s, ABOUT, 1.0);
        // ABOUT removed from the requested order (and it's unknown-filtered too,
        // but here we just drop it from order via a restack that omits it).
        let e = reduce(&mut s, SceneEvent::Restack(vec![MAIN]));
        assert!(e.contains(&Effect::MenuDestroy));
        assert!(e.contains(&Effect::Fire(-1)));
        assert!(!s.menu_active());
    }

    #[test]
    fn menu_motion_into_row_redraws_scaled() {
        let mut s = Scene::default();
        add(&mut s, ABOUT);
        show_menu(&mut s, ABOUT, 2.0);
        // origin=(200,100), scale=2: logical (220,104) → local (40,8) → row 0
        // (local x must stay < width 100, so x_log < 250).
        let e = reduce(&mut s, SceneEvent::MenuPointerMotion { x: 220, y: 104 });
        assert_eq!(e, vec![Effect::MenuRedraw]);
    }

    #[test]
    fn menu_press_on_row_selects_and_destroys() {
        let mut s = Scene::default();
        add(&mut s, ABOUT);
        show_menu(&mut s, ABOUT, 1.0);
        // origin=(200,100): logical (250,118) → local (50,18) → row 1 (id 20).
        let e = reduce(&mut s, SceneEvent::MenuPointerPress { x: 250, y: 118 });
        assert_eq!(e, vec![Effect::MenuDestroy, Effect::Fire(20)]);
        assert!(!s.menu_active());
    }

    #[test]
    fn menu_press_outside_dismisses() {
        let mut s = Scene::default();
        add(&mut s, ABOUT);
        show_menu(&mut s, ABOUT, 1.0);
        let e = reduce(&mut s, SceneEvent::MenuPointerPress { x: 9999, y: 9999 });
        assert_eq!(e, vec![Effect::MenuDestroy, Effect::Fire(-1)]);
    }

    #[test]
    fn menu_escape_dismisses() {
        let mut s = Scene::default();
        add(&mut s, ABOUT);
        show_menu(&mut s, ABOUT, 1.0);
        let e = reduce(&mut s, SceneEvent::MenuKey(interaction_fsm::XK_ESCAPE));
        assert_eq!(e, vec![Effect::MenuDestroy, Effect::Fire(-1)]);
    }

    #[test]
    fn menu_input_without_menu_is_noop() {
        let mut s = Scene::default();
        assert_eq!(
            reduce(&mut s, SceneEvent::MenuPointerMotion { x: 1, y: 1 }),
            vec![]
        );
        assert_eq!(
            reduce(&mut s, SceneEvent::MenuKey(interaction_fsm::XK_ESCAPE)),
            vec![]
        );
        assert_eq!(reduce(&mut s, SceneEvent::MenuDismiss), vec![]);
    }

    #[test]
    fn second_menu_show_replaces_first() {
        let mut s = Scene::default();
        add(&mut s, ABOUT);
        show_menu(&mut s, ABOUT, 1.0);
        let e = show_menu(&mut s, ABOUT, 1.0);
        assert_eq!(
            e,
            vec![Effect::MenuDestroy, Effect::Fire(-1), Effect::MenuCreate]
        );
    }
}
