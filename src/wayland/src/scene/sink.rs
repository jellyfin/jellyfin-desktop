//! Effect interpreters for [`super::reduce`].
//!
//! [`SceneSink`] is the seam: production uses [`WlSink`] (real Wayland IO);
//! tests use [`RecordingSink`] to assert the effect stream a driver would
//! execute, with no compositor.

use wayland_client::protocol::wl_subsurface::WlSubsurface;
use wayland_client::protocol::wl_surface::WlSurface;

use super::{Above, Effect, LayerId};
use crate::wl_state::{PlatformSurface, WlState};

/// Applies a stream of [`Effect`]s. Production performs Wayland IO; the test
/// double records.
pub trait SceneSink {
    fn apply(&mut self, effect: &Effect);
}

/// Test double: records effects in order.
#[cfg(test)]
#[derive(Default)]
pub struct RecordingSink {
    pub effects: Vec<Effect>,
}

#[cfg(test)]
impl SceneSink for RecordingSink {
    fn apply(&mut self, effect: &Effect) {
        self.effects.push(*effect);
    }
}

fn layer_ptr(id: LayerId) -> *mut PlatformSurface {
    id.0 as *mut PlatformSurface
}

/// Clone the (subsurface, surface) proxies for a layer id, if live.
fn layer_objs(id: LayerId) -> Option<(WlSubsurface, WlSurface)> {
    let p = layer_ptr(id);
    if p.is_null() {
        return None;
    }
    // SAFETY: LayerId is a live PlatformSurface address (removed from the scene
    // before the box is freed), dereferenced only under the wl_state lock.
    let s = unsafe { &*p };
    match (s.subsurface.as_ref(), s.surface.as_ref()) {
        (Some(sub), Some(surf)) => Some((sub.clone(), surf.clone())),
        _ => None,
    }
}

pub struct WlSink<'a> {
    st: &'a mut WlState,
}

impl<'a> WlSink<'a> {
    pub fn new(st: &'a mut WlState) -> Self {
        Self { st }
    }

    fn place_above(&mut self, layer: LayerId, above: Above) {
        let Some((sub, _)) = layer_objs(layer) else {
            return;
        };
        match above {
            Above::Parent => sub.place_above(&self.st.parent),
            Above::Layer(p) => {
                if let Some((_, surf)) = layer_objs(p) {
                    sub.place_above(&surf);
                }
            }
        }
    }
}

impl SceneSink for WlSink<'_> {
    fn apply(&mut self, effect: &Effect) {
        match *effect {
            Effect::PlaceAbove { layer, above } => self.place_above(layer, above),
            Effect::CommitParent => self.st.parent.commit(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::{Above, Effect, LayerId, Scene, SceneEvent, reduce};
    use super::{RecordingSink, SceneSink};

    /// The driver feeds reduce's effects into a sink unchanged; with a
    /// RecordingSink we can assert the exact IO sequence headlessly.
    #[test]
    fn recording_sink_captures_add_sequence() {
        let mut scene = Scene::default();
        let mut sink = RecordingSink::default();
        for ev in [
            SceneEvent::LayerAdded(LayerId(1)),
            SceneEvent::LayerAdded(LayerId(2)),
        ] {
            for e in reduce(&mut scene, ev) {
                sink.apply(&e);
            }
        }
        assert_eq!(
            sink.effects,
            vec![
                // LayerAdded(1): stack + commit.
                Effect::PlaceAbove {
                    layer: LayerId(1),
                    above: Above::Parent
                },
                Effect::CommitParent,
                // LayerAdded(2): full rebuild + commit.
                Effect::PlaceAbove {
                    layer: LayerId(1),
                    above: Above::Parent
                },
                Effect::PlaceAbove {
                    layer: LayerId(2),
                    above: Above::Layer(LayerId(1))
                },
                Effect::CommitParent,
            ]
        );
    }
}
