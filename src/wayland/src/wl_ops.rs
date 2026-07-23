//! Surface lifecycle + paint ops.
//!
//! All entry points run under the [`wl_state::lock()`] mutex. Each
//! protocol-touching op calls `WlState::flush()` (or `conn.flush()`)
//! before returning so commits land in compositor order matching the
//! C++ original.

use jfn_platform_abi::JfnRect;
use std::os::fd::{AsFd, OwnedFd};
use wayland_client::protocol::wl_surface::WlSurface;
use wayland_protocols::wp::viewporter::client::wp_viewport::WpViewport;

use crate::layer::{LayerSurface, Present, PresentError, SurfaceRef, ViewportState};
use crate::layer_actor::{LayerActor, LayerBackend};
use crate::wl_state::{
    PlatformSurface, WlState, create_dmabuf_buffer, create_shm_buffer, lock, size_in_tolerance,
};

// =====================================================================
// Lifetime helpers
// =====================================================================

/// The returned pointer is stable for the surface's lifetime; the caller owns
/// it until `free_surface`.
fn new_boxed() -> *mut PlatformSurface {
    Box::into_raw(Box::new(PlatformSurface::new()))
}

unsafe fn drop_boxed(p: *mut PlatformSurface) {
    if !p.is_null() {
        drop(unsafe { Box::from_raw(p) });
    }
}

unsafe fn surface_mut<'a>(p: *mut PlatformSurface) -> &'a mut PlatformSurface {
    unsafe { &mut *p }
}

// =====================================================================
// alloc / free / restack
// =====================================================================

pub(crate) fn alloc_surface() -> *mut PlatformSurface {
    let ptr = new_boxed();
    let mut st = lock();
    // SAFETY: ptr is freshly heap-allocated; no aliases yet.
    let s = unsafe { surface_mut(ptr) };

    let surface = st.compositor.create_surface(&st.qh, ());

    // No input region on subsurface — keystrokes/clicks go to parent only.
    let empty = st.compositor.create_region(&st.qh, ());
    surface.set_input_region(Some(&empty));
    empty.destroy();

    let viewport = st
        .viewporter
        .as_ref()
        .map(|vp| vp.get_viewport(&surface, &st.qh, ()));

    surface.commit();
    st.flush();

    s.layer_actor = Some(build_actor(&st, &surface, &viewport, s.visible));
    s.surface = Some(SurfaceRef::new(surface, viewport));
    crate::wl_state::parent_layer(&mut st, ptr);

    crate::scene::dispatch(
        &mut st,
        crate::scene::SceneEvent::LayerAdded(crate::scene::LayerId(ptr as usize)),
    );
    drop(st);
    crate::root_window::request_present();
    ptr
}

pub(crate) fn free_surface(ptr: *mut PlatformSurface) {
    if ptr.is_null() {
        return;
    }

    // Shut the actor down before taking the lock: Vulkan WSI swapchain teardown
    // dispatches Wayland events, which would deadlock against the held lock.
    {
        let s = unsafe { surface_mut(ptr) };
        if let Some(actor) = s.layer_actor.take() {
            actor.shutdown();
        }
    }

    {
        let mut st = lock();
        // Drop from stack if still present.
        st.stack.retain(|p| *p != ptr);

        // Update the scene before tearing down wl objects: dismissing a menu
        // anchored here requires this layer's surface to still be alive.
        crate::scene::dispatch(
            &mut st,
            crate::scene::SceneEvent::LayerRemoved(crate::scene::LayerId(ptr as usize)),
        );

        // SAFETY: stack drop above guarantees no aliases via stack;
        // caller (C++) guarantees no concurrent use of `ptr`.
        let s = unsafe { surface_mut(ptr) };
        popup_destroy_locked(s);
        if let Some(sub) = s.subsurface.take() {
            sub.destroy();
        }
        if let Some(surface) = s.surface.take() {
            surface.destroy();
        }
        st.flush();
    }
    unsafe { drop_boxed(ptr) };
}

pub(crate) fn restack(ordered: &[*mut PlatformSurface]) {
    let mut st = lock();
    st.stack.clear();
    st.stack.extend_from_slice(ordered);
    let order: Vec<crate::scene::LayerId> = ordered
        .iter()
        .filter(|p| !p.is_null())
        .map(|p| crate::scene::LayerId(*p as usize))
        .collect();
    crate::scene::dispatch(&mut st, crate::scene::SceneEvent::Restack(order));
}

// =====================================================================
// set_visible
// =====================================================================

pub(crate) fn surface_set_visible(
    ptr: *mut PlatformSurface,
    visible: bool,
    bg_r: u8,
    bg_g: u8,
    bg_b: u8,
) {
    if ptr.is_null() {
        return;
    }
    let st = lock();
    let s = unsafe { surface_mut(ptr) };
    if s.visible == visible {
        return;
    }
    s.visible = visible;
    if s.surface.is_none() {
        return;
    }

    // Skip the placeholder in GPU mode: Vulkan-WSI owns this surface's buffers,
    // so an shm placeholder would fight the swapchain.
    let use_gpu_paint = st.use_gpu_paint;
    if let Some(actor) = s.layer_actor.as_ref() {
        actor.set_visible(visible);
        if visible && !use_gpu_paint {
            actor.request_placeholder(bg_r, bg_g, bg_b);
        }
    }
    s.null_attached = !visible;
    crate::root_window::request_present();
}

// =====================================================================
// Popup
// =====================================================================

pub(crate) fn popup_show(ptr: *mut PlatformSurface, x: i32, y: i32, lw: i32, lh: i32) {
    if ptr.is_null() {
        return;
    }
    let st = lock();
    let s = unsafe { surface_mut(ptr) };
    popup_create_locked(s, &st);
    s.popup_visible = true;
    let Some(sub) = s.popup_subsurface.as_ref() else {
        return;
    };
    sub.set_position(x, y);
    if let Some(vp) = s.popup_viewport.as_ref()
        && lw > 0
        && lh > 0
    {
        vp.set_destination(lw, lh);
    }
    st.flush();
}

pub(crate) fn popup_hide(ptr: *mut PlatformSurface) {
    if ptr.is_null() {
        return;
    }
    let st = lock();
    let s = unsafe { surface_mut(ptr) };
    s.popup_visible = false;
    // Drain any popup commit the worker still owes before destroying the popup
    // surface: committing the popup proxy after it is destroyed aborts the client.
    // Holding the `wl_state` lock blocks any new popup enqueue, so once drained
    // nothing more can be owed.
    if let Some(actor) = s.layer_actor.as_ref() {
        actor.drain_popup();
    }
    popup_destroy_locked(s);
    st.flush();
}

fn popup_create_locked(s: &mut PlatformSurface, st: &WlState) {
    let Some(parent) = s.surface.as_ref() else {
        return;
    };
    if s.popup_surface.is_some() {
        return;
    }
    let surf = st.compositor.create_surface(&st.qh, ());
    let sub =
        crate::wl_state::SyncSubsurface::create(&st.subcompositor, &surf, parent.as_arg(), &st.qh);
    let empty = st.compositor.create_region(&st.qh, ());
    surf.set_input_region(Some(&empty));
    empty.destroy();
    let vp = st
        .viewporter
        .as_ref()
        .map(|v| v.get_viewport(&surf, &st.qh, ()));
    s.popup_surface = Some(surf);
    s.popup_subsurface = Some(sub);
    s.popup_viewport = vp;
}

fn popup_destroy_locked(s: &mut PlatformSurface) {
    if let Some(v) = s.popup_viewport.take() {
        v.destroy();
    }
    if let Some(b) = s.popup_buffer.take() {
        crate::wl_state::retire_buffer(b);
    }
    if let Some(sub) = s.popup_subsurface.take() {
        sub.destroy();
    }
    if let Some(surf) = s.popup_surface.take() {
        surf.destroy();
    }
}

// =====================================================================
// Present (dmabuf / software)
// =====================================================================

/// Frame info the caller unpacks from CefAcceleratedPaintInfo. Owns its
/// dup'd dmabuf fd so it's closed on drop after the buffer is built —
/// the compositor dups its own copy over the wire in `create_params.add`.
pub struct JfnDmabufFrame {
    pub fd: OwnedFd,
    pub id: Option<(u64, u64)>,
    pub stride: u32,
    pub modifier: u64,
    pub coded_w: i32,
    pub coded_h: i32,
    pub visible_w: i32,
    pub visible_h: i32,
}

fn build_actor(
    st: &WlState,
    surface: &WlSurface,
    viewport: &Option<WpViewport>,
    visible: bool,
) -> LayerActor {
    let backend = match (st.use_gpu_paint, st.gpu_ctx.clone()) {
        (true, Some(ctx)) => LayerBackend::Gpu(ctx),
        _ => LayerBackend::Shm,
    };
    let (lw, lh, pw, ph) = extent_or(0, 0);
    let layer = LayerSurface::new(st.conn.clone(), surface.clone(), viewport.clone());
    LayerActor::new(
        backend,
        st.qh.clone(),
        st.shm.clone(),
        st.dmabuf.clone(),
        layer,
        ViewportState { lw, lh, pw, ph },
        visible,
    )
}

fn extent_or(w: i32, h: i32) -> (i32, i32, i32, i32) {
    crate::window_state::window_extent().map_or((w, h, w, h), |ext| {
        (
            ext.logical().w(),
            ext.logical().h(),
            ext.physical().w(),
            ext.physical().h(),
        )
    })
}

pub(crate) fn surface_present(
    ptr: *mut PlatformSurface,
    frame: JfnDmabufFrame,
) -> Result<Present, PresentError> {
    if ptr.is_null() {
        return Ok(Present::Skipped);
    }
    let (w, h) = (frame.coded_w, frame.coded_h);
    let (vw, vh) = (frame.visible_w, frame.visible_h);

    let st = lock();
    let s = unsafe { surface_mut(ptr) };
    if s.surface.is_none() || !s.visible || st.dmabuf.is_none() {
        return Ok(Present::Skipped);
    }
    if !size_in_tolerance(vw, vh) && !s.null_attached {
        return Ok(Present::Skipped);
    }

    s.null_attached = false;
    let (lw, lh, pw, ph) = extent_or(w, h);

    let Some(actor) = s.layer_actor.as_ref() else {
        return Ok(Present::Skipped);
    };
    actor.set_visible(s.visible);
    actor.resize(lw, lh, pw, ph);
    actor.present_dmabuf(frame)
}

pub(crate) fn surface_present_software(
    ptr: *mut PlatformSurface,
    dirty: &[JfnRect],
    pixels: &[u8],
    w: i32,
    h: i32,
) -> Result<Present, PresentError> {
    if ptr.is_null() || w <= 0 || h <= 0 {
        return Err(PresentError::BadDimensions(w, h));
    }

    let _st = lock();
    let s = unsafe { surface_mut(ptr) };
    if s.surface.is_none() || !s.visible {
        return Ok(Present::Skipped);
    }

    s.null_attached = false;
    let (lw, lh, pw, ph) = extent_or(w, h);

    let Some(actor) = s.layer_actor.as_ref() else {
        return Ok(Present::Skipped);
    };
    actor.set_visible(s.visible);
    actor.resize(lw, lh, pw, ph);
    actor.present_software(pixels, w, h, dirty)
}

pub(crate) fn popup_present(ptr: *mut PlatformSurface, frame: &JfnDmabufFrame, lw: i32, lh: i32) {
    if ptr.is_null() || lw <= 0 || lh <= 0 {
        return;
    }
    let st = lock();
    let s = unsafe { surface_mut(ptr) };
    if s.popup_surface.is_none() || !s.popup_visible {
        return;
    }
    let w = frame.coded_w;
    let h = frame.coded_h;
    let vw = if frame.visible_w > 0 {
        frame.visible_w
    } else {
        w
    };
    let vh = if frame.visible_h > 0 {
        frame.visible_h
    } else {
        h
    };
    let Some(dmabuf) = st.dmabuf.as_ref() else {
        return;
    };
    let Some(buf) = create_dmabuf_buffer(
        dmabuf,
        &st.qh,
        frame.fd.as_fd(),
        frame.stride,
        frame.modifier,
        w,
        h,
    ) else {
        return;
    };
    if let Some(old) = s.popup_buffer.take() {
        crate::wl_state::retire_buffer(old);
    }
    if let Some(vp) = s.popup_viewport.as_ref() {
        vp.set_source(0.0, 0.0, vw as f64, vh as f64);
        vp.set_destination(lw, lh);
    }
    let Some(popup) = s.popup_surface.as_ref() else {
        return;
    };
    buf.attach_to(popup, 0, 0);
    popup.damage_buffer(0, 0, vw, vh);
    commit_popup_via_actor(s, popup, &st);
    s.popup_buffer = Some(buf);
}

pub(crate) fn popup_present_software(
    ptr: *mut PlatformSurface,
    pixels: &[u8],
    pw: i32,
    ph: i32,
    lw: i32,
    lh: i32,
) {
    if ptr.is_null() || lw <= 0 || lh <= 0 {
        return;
    }
    let st = lock();
    let s = unsafe { surface_mut(ptr) };
    if s.popup_surface.is_none() || !s.popup_visible {
        return;
    }
    let Some(buf) = create_shm_buffer(&st, pixels, pw, ph) else {
        return;
    };
    if let Some(old) = s.popup_buffer.take() {
        crate::wl_state::retire_buffer(old);
    }
    if let Some(vp) = s.popup_viewport.as_ref() {
        vp.set_source(0.0, 0.0, pw as f64, ph as f64);
        vp.set_destination(lw, lh);
    }
    let Some(popup) = s.popup_surface.as_ref() else {
        return;
    };
    buf.attach_to(popup, 0, 0);
    popup.damage_buffer(0, 0, pw, ph);
    commit_popup_via_actor(s, popup, &st);
    s.popup_buffer = Some(buf);
}

/// Route the popup commit through the actor so it lands ordered after the
/// layer's own commits; commit inline only when the surface has no actor.
fn commit_popup_via_actor(s: &PlatformSurface, popup: &WlSurface, st: &WlState) {
    match s.layer_actor.as_ref() {
        Some(actor) => actor.commit_popup(popup.clone()),
        None => {
            popup.commit();
            st.flush();
            crate::root_window::request_present();
        }
    }
}

pub(crate) fn on_configure(fullscreen: bool) {
    let Some(ext) = crate::window_state::window_extent() else {
        return;
    };
    let (lw, lh) = (ext.logical().w(), ext.logical().h());
    let (pw, ph) = (ext.physical().w(), ext.physical().h());

    let mut st = lock();

    st.was_fullscreen = fullscreen;

    crate::wl_state::ensure_root_locked(&mut st);

    for &p in &st.stack {
        if p.is_null() {
            continue;
        }
        let s = unsafe { surface_mut(p) };
        if let Some(actor) = s.layer_actor.as_ref() {
            actor.resize(lw, lh, pw, ph);
        }
    }

    st.flush();
}
