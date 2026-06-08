//! Effect interpreters for [`super::reduce`].
//!
//! [`SceneSink`] is the seam: production uses [`WlSink`] (real Wayland IO);
//! tests use [`RecordingSink`] to assert the effect stream a driver would
//! execute, with no compositor. The menu's IO resources (fonts, wl objects,
//! pending selection callback) live in [`MenuIo`], owned by `WlState` so they
//! persist across dispatches; the reducer keeps only the pure menu model.

use wayland_client::protocol::wl_buffer::WlBuffer;
use wayland_client::protocol::wl_subsurface::WlSubsurface;
use wayland_client::protocol::wl_surface::WlSurface;
use wayland_protocols::wp::viewporter::client::wp_viewport::WpViewport;

use jfn_menu::MenuItem;
use jfn_menu::render::{self, Fonts, Layout};

use super::{Above, Effect, LayerId};
use crate::wl_state::{PlatformSurface, WlState, create_shm_buffer};

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

/// Persistent IO backing for the menu (not part of the pure scene).
#[derive(Default)]
pub struct MenuIo {
    fonts: Option<Fonts>,
    objects: Option<MenuObjects>,
    pending: Option<Box<dyn FnOnce(i32) + Send>>,
}

impl MenuIo {
    /// Stash the selection callback before a `MenuShow` dispatch.
    pub fn arm(&mut self, cb: Box<dyn FnOnce(i32) + Send>) {
        self.pending = Some(cb);
    }

    /// Lazily-created shared font system (glyph cache reused across menus).
    pub fn fonts_mut(&mut self) -> &mut Fonts {
        self.fonts.get_or_insert_with(Fonts::new)
    }
}

struct MenuObjects {
    surface: WlSurface,
    subsurface: WlSubsurface,
    viewport: Option<WpViewport>,
    buffer: Option<WlBuffer>,
    parent_layer: WlSurface,
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

    fn menu_create(&mut self) {
        let st = &mut *self.st;
        let Some(menu) = st.scene.menu.as_ref() else {
            return;
        };
        let Some((_, parent_layer)) = layer_objs(menu.parent) else {
            return;
        };
        let (origin, pw, ph, scale) = (
            menu.origin,
            menu.layout.width,
            menu.layout.height,
            menu.layout.scale(),
        );

        // Paint with no active row. Holds &mut fonts + &menu (disjoint fields);
        // both borrows end before we touch the rest of WlState below.
        let pixels = {
            let fonts = st.menu_io.fonts.get_or_insert_with(Fonts::new);
            paint_bgra(fonts, &menu.layout, &menu.items, -1)
        };
        let Some(pixels) = pixels else { return };

        let lw = logical_dim(pw, scale);
        let lh = logical_dim(ph, scale);
        let surface = st.compositor.create_surface(&st.qh, ());
        let subsurface = st
            .subcompositor
            .get_subsurface(&surface, &parent_layer, &st.qh, ());
        subsurface.set_desync();
        let empty = st.compositor.create_region(&st.qh, ());
        surface.set_input_region(Some(&empty));
        empty.destroy();
        subsurface.set_position(origin.0, origin.1);
        subsurface.place_above(&parent_layer);
        let viewport = st
            .viewporter
            .as_ref()
            .map(|v| v.get_viewport(&surface, &st.qh, ()));
        let buffer = create_shm_buffer(st, &pixels, pw, ph);
        if let (Some(vp), Some(_)) = (viewport.as_ref(), buffer.as_ref()) {
            vp.set_source(0.0, 0.0, pw as f64, ph as f64);
            vp.set_destination(lw, lh);
        }
        if let Some(buf) = buffer.as_ref() {
            surface.attach(Some(buf), 0, 0);
            surface.damage_buffer(0, 0, pw, ph);
        }
        surface.commit();
        parent_layer.commit();
        st.menu_io.objects = Some(MenuObjects {
            surface,
            subsurface,
            viewport,
            buffer,
            parent_layer,
        });
    }

    fn menu_redraw(&mut self) {
        let st = &mut *self.st;
        let Some(menu) = st.scene.menu.as_ref() else {
            return;
        };
        let (pw, ph, active) = (menu.layout.width, menu.layout.height, menu.state.active);
        let pixels = {
            let fonts = st.menu_io.fonts.get_or_insert_with(Fonts::new);
            paint_bgra(fonts, &menu.layout, &menu.items, active)
        };
        let Some(pixels) = pixels else { return };
        let Some(buf) = create_shm_buffer(st, &pixels, pw, ph) else {
            return;
        };
        let Some(obj) = st.menu_io.objects.as_mut() else {
            return;
        };
        if let Some(old) = obj.buffer.take() {
            old.destroy();
        }
        obj.surface.attach(Some(&buf), 0, 0);
        obj.surface.damage_buffer(0, 0, pw, ph);
        obj.surface.commit();
        obj.parent_layer.commit();
        obj.buffer = Some(buf);
    }

    fn menu_destroy(&mut self) {
        let Some(mut obj) = self.st.menu_io.objects.take() else {
            return;
        };
        if let Some(vp) = obj.viewport.take() {
            vp.destroy();
        }
        if let Some(b) = obj.buffer.take() {
            b.destroy();
        }
        obj.subsurface.destroy();
        obj.surface.destroy();
        obj.parent_layer.commit();
    }

    fn fire(&mut self, id: i32) {
        if let Some(cb) = self.st.menu_io.pending.take() {
            cb(id);
        }
    }
}

impl SceneSink for WlSink<'_> {
    fn apply(&mut self, effect: &Effect) {
        match *effect {
            Effect::PlaceAbove { layer, above } => self.place_above(layer, above),
            Effect::CommitParent => self.st.parent.commit(),
            Effect::MenuCreate => self.menu_create(),
            Effect::MenuRedraw => self.menu_redraw(),
            Effect::MenuDestroy => self.menu_destroy(),
            Effect::Fire(id) => self.fire(id),
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
