//! Pure model of the Wayland surface tree's layer stacking order.
//!
//! Wayland subsurface placement is parent-double-buffered: every stacking
//! change must be followed by a [`Effect::CommitParent`] or the new z-order
//! silently never applies.

pub mod sink;

use crate::wl_state::WlState;
use sink::SceneSink;

/// Opaque layer identity; in production a `*mut PlatformSurface` address, only
/// ever compared, never dereferenced by the reducer.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)]
pub struct LayerId(pub usize);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Above {
    Parent,
    Layer(LayerId),
}

#[derive(Default)]
pub struct Scene {
    order: Vec<LayerId>,
    applied: Vec<LayerId>,
}

pub enum SceneEvent {
    LayerAdded(LayerId),
    LayerRemoved(LayerId),
    Restack(Vec<LayerId>),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Effect {
    PlaceAbove { layer: LayerId, above: Above },
    CommitParent,
}

impl Scene {
    fn has(&self, id: LayerId) -> bool {
        self.order.contains(&id)
    }

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
            scene.order.retain(|&l| l != id);
            scene.restack_effects()
        }
        SceneEvent::Restack(order) => {
            let known: Vec<LayerId> = order.into_iter().filter(|id| scene.has(*id)).collect();
            scene.order = known;
            scene.restack_effects()
        }
    }
}

pub(crate) fn dispatch(st: &mut WlState, ev: SceneEvent) {
    let effects = reduce(&mut st.scene, ev);
    {
        let mut s = sink::WlSink::new(st);
        for e in &effects {
            s.apply(e);
        }
    }
    st.flush();
}

#[cfg(test)]
mod tests {
    use super::*;

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

    /// Any event that changes the order must end in exactly one CommitParent,
    /// else the new z-order never applies (parent-double-buffered placement).
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
}
