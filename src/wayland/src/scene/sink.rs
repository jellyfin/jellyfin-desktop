//! Effect interpreters for [`super::reduce`].

use wayland_client::protocol::wl_surface::WlSurface;

use super::{Above, Effect, LayerId};
use crate::wl_state::PlatformSurface;

pub trait SceneSink {
    fn apply(&mut self, effect: &Effect);
}

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

// The synchronized subsurface stays owned by its PlatformSurface (the raw object
// never escapes); only the sibling surface handle is cloned out for restacking.
fn layer_surface(id: LayerId) -> Option<WlSurface> {
    let p = layer_ptr(id);
    if p.is_null() {
        return None;
    }
    // SAFETY: LayerId is a live PlatformSurface address (removed from the scene
    // before the box is freed), dereferenced only under the wl_state lock.
    let s = unsafe { &*p };
    s.surface.as_ref().map(|sr| sr.as_arg().clone())
}

pub struct WlSink;

impl WlSink {
    pub fn new() -> Self {
        Self
    }

    fn place_above(&mut self, layer: LayerId, above: Above) {
        let p = layer_ptr(layer);
        if p.is_null() {
            return;
        }
        // SAFETY: see `layer_surface` — live address, accessed under the lock.
        let s = unsafe { &*p };
        let Some(sub) = s.subsurface.as_ref() else {
            return;
        };
        match above {
            // Empty by design: re-stacking the bottom layer onto the root would
            // sink it below mpv.
            Above::Parent => {}
            Above::Layer(pp) => {
                if let Some(surf) = layer_surface(pp) {
                    sub.place_above(&surf);
                }
            }
        }
    }
}

impl Default for WlSink {
    fn default() -> Self {
        Self::new()
    }
}

impl SceneSink for WlSink {
    fn apply(&mut self, effect: &Effect) {
        match *effect {
            Effect::PlaceAbove { layer, above } => self.place_above(layer, above),
            Effect::CommitParent => crate::root_window::request_present(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::{Above, Effect, LayerId, Scene, SceneEvent, reduce};
    use super::{RecordingSink, SceneSink};

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
                Effect::PlaceAbove {
                    layer: LayerId(1),
                    above: Above::Parent
                },
                Effect::CommitParent,
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
