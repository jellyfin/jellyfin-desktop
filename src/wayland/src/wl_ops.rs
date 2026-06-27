//! Surface lifecycle + paint + transition ops.
//!
//! All entry points run under the [`wl_state::lock()`] mutex. Each
//! protocol-touching op calls `WlState::flush()` (or `conn.flush()`)
//! before returning so commits land in compositor order matching the
//! C++ original.

use jfn_gpu_paint::DirtyRect;
use jfn_platform_abi::JfnRect;
use std::os::fd::{AsFd, OwnedFd};
use wayland_client::Proxy;
use wayland_client::protocol::wl_subsurface::WlSubsurface;

use crate::gpu_paint_worker::WaylandGpuPaintWorker;
use crate::shm_paint_worker::{ViewportState, WaylandShmPaintWorker};
use crate::wl_state::{
    PlatformSurface, PresentMode, WlState, create_dmabuf_buffer, create_shm_buffer,
    create_solid_color_buffer, lock, size_in_tolerance,
};

// =====================================================================
// Lifetime helpers
// =====================================================================

/// Heap-allocate a fresh PlatformSurface and return its raw pointer.
/// Caller owns it until `free_surface` is invoked. The pointer is
/// stable for the surface's lifetime.
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
    let subsurface = st
        .subcompositor
        .get_subsurface(&surface, &st.parent, &st.qh, ());
    subsurface.set_desync();

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

    s.surface = Some(surface);
    s.subsurface = Some(subsurface);
    s.viewport = viewport;

    crate::scene::dispatch(
        &mut st,
        crate::scene::SceneEvent::LayerAdded(crate::scene::LayerId(ptr as usize)),
    );
    ptr
}

pub(crate) fn free_surface(ptr: *mut PlatformSurface) {
    if ptr.is_null() {
        return;
    }

    // Tear down the GPU paint worker outside the lock — Vulkan WSI swapchain
    // destruction can roundtrip/dispatch Wayland events. Caller (CEF UI
    // thread) owns this ptr exclusively; the worker field can be safely taken
    // via a raw deref before grabbing the lock.
    {
        let s = unsafe { surface_mut(ptr) };
        if let Some(worker) = s.gpu_paint_worker.take() {
            worker.shutdown();
        }
        if let Some(worker) = s.shm_paint_worker.take() {
            worker.shutdown();
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
        if let Some(v) = s.viewport.take() {
            v.destroy();
        }
        if let Some(b) = s.buffer.take() {
            b.destroy();
        }
        if let Some(sub) = s.subsurface.take() {
            sub.destroy();
        }
        if let Some(surf) = s.surface.take() {
            surf.destroy();
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
// resize / set_visible
// =====================================================================

pub(crate) fn surface_resize(ptr: *mut PlatformSurface, lw: i32, lh: i32, pw: i32, ph: i32) {
    if ptr.is_null() {
        return;
    }
    let st = lock();
    let s = unsafe { surface_mut(ptr) };
    s.lw = lw;
    s.lh = lh;
    s.pw = pw;
    s.ph = ph;

    // Vulkan-WSI path: record desired size/viewport state and notify the
    // presenter worker. The callback never performs wgpu work.
    if st.use_gpu_paint {
        set_viewport_for_buffer_locked(s, s.buffer_w, s.buffer_h);
        if let Some(worker) = s.gpu_paint_worker.as_ref() {
            worker.resize((pw.max(1) as u32, ph.max(1) as u32));
        }
        st.flush();
        return;
    }
    if let Some(worker) = s.shm_paint_worker.as_ref() {
        worker.resize(lw, lh, pw, ph);
        return;
    }

    let Some(surface) = s.surface.as_ref() else {
        return;
    };
    let Some(viewport) = s.viewport.as_ref() else {
        return;
    };
    let is_main = st.stack.first().map(|p| *p == ptr).unwrap_or(false);
    if st.transitioning && is_main {
        viewport.set_destination(lw, lh);
    } else if s.buffer_w > 0 && s.buffer_h > 0 && pw > 0 && ph > 0 {
        let src_w = s.buffer_w.min(pw);
        let src_h = s.buffer_h.min(ph);
        let dst_w = src_w * lw / pw;
        let dst_h = src_h * lh / ph;
        viewport.set_source(0.0, 0.0, src_w as f64, src_h as f64);
        viewport.set_destination(dst_w, dst_h);
    } else {
        viewport.set_destination(lw, lh);
    }
    surface.commit();
    st.flush();
}

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
    let Some(surface) = s.surface.clone() else {
        return;
    };

    // Vulkan-WSI owns attach/commit on this surface — skip the placeholder
    // and the null-attach. Notify the presenter worker without doing wgpu
    // work on this callback.
    if st.use_gpu_paint {
        if let Some(worker) = s.gpu_paint_worker.as_ref() {
            worker.set_visible(visible);
        }
        return;
    }
    if let Some(worker) = s.shm_paint_worker.as_ref() {
        worker.set_visible(visible);
        if !visible {
            surface.attach(None, 0, 0);
            surface.commit();
            st.flush();
            s.null_attached = true;
        }
        return;
    }

    if visible {
        // Solid-color placeholder so the user sees the theme background
        // before CEF's first paint lands.
        if let Some(buf) = create_solid_color_buffer(&st, bg_r, bg_g, bg_b) {
            if let Some(old) = s.buffer.take() {
                old.destroy();
            }
            s.placeholder = true;
            if let Some(viewport) = s.viewport.as_ref() {
                viewport.set_source(0.0, 0.0, 1.0, 1.0);
            }
            surface.attach(Some(&buf), 0, 0);
            surface.damage_buffer(0, 0, 1, 1);
            surface.commit();
            st.flush();
            s.buffer = Some(buf);
            s.null_attached = false;
        }
    } else {
        surface.attach(None, 0, 0);
        surface.commit();
        st.flush();
        if let Some(b) = s.buffer.take() {
            b.destroy();
        }
        s.placeholder = false;
        s.null_attached = true;
    }
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
    let sub: WlSubsurface = st.subcompositor.get_subsurface(&surf, parent, &st.qh, ());
    sub.set_desync();
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
        b.destroy();
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
    pub stride: u32,
    pub modifier: u64,
    pub coded_w: i32,
    pub coded_h: i32,
    pub visible_w: i32,
    pub visible_h: i32,
}

pub(crate) fn surface_present(ptr: *mut PlatformSurface, frame: &JfnDmabufFrame) -> bool {
    if ptr.is_null() {
        return false;
    }
    let w = frame.coded_w;
    let h = frame.coded_h;
    let vw = frame.visible_w;
    let vh = frame.visible_h;

    let mut st = lock();
    let s = unsafe { surface_mut(ptr) };
    if s.surface.is_none() || !s.visible || st.dmabuf.is_none() {
        return false;
    }
    if st.present_mode == PresentMode::Drop {
        return false;
    }
    if st.transitioning && !size_in_tolerance(s, vw, vh) {
        unmap_locked(s);
        st.flush();
        return false;
    }

    let Some(buf) = create_dmabuf_buffer(&st, frame.fd.as_fd(), frame.stride, frame.modifier, w, h)
    else {
        return false;
    };

    if st.transitioning && !size_in_tolerance(s, vw, vh) {
        buf.destroy();
        unmap_locked(s);
        st.flush();
        return false;
    }

    let was_transitioning = st.transitioning;
    let was_null_attached = s.null_attached;
    if !was_transitioning && s.pw > 0 && !size_in_tolerance(s, vw, vh) && !was_null_attached {
        buf.destroy();
        return false;
    }

    attach_and_commit_locked(s, buf, w, h);
    st.flush();

    if was_transitioning {
        // First in-tolerance frame ends the FS transition.
        st.transitioning = false;
    }
    true
}

fn queue_shm_present(
    s: &mut PlatformSurface,
    st: &WlState,
    dirty: &[JfnRect],
    pixels: &[u8],
    w: i32,
    h: i32,
) -> bool {
    let Some(surface) = s.surface.as_ref() else {
        return false;
    };
    s.buffer_w = w;
    s.buffer_h = h;
    s.placeholder = false;
    s.null_attached = false;

    if s.shm_paint_worker.is_none() {
        s.shm_paint_worker = Some(WaylandShmPaintWorker::new(
            st.conn.clone(),
            st.qh.clone(),
            st.shm.clone(),
            surface.clone(),
            s.viewport.clone(),
            ViewportState {
                lw: s.lw,
                lh: s.lh,
                pw: s.pw,
                ph: s.ph,
            },
            s.visible,
        ));
    }

    let Some(worker) = s.shm_paint_worker.as_ref() else {
        return false;
    };
    worker.set_visible(s.visible);
    worker.resize(s.lw, s.lh, s.pw, s.ph);
    worker.submit_frame(pixels, w, h, dirty)
}

pub(crate) fn surface_present_software(
    ptr: *mut PlatformSurface,
    dirty: &[JfnRect],
    pixels: &[u8],
    w: i32,
    h: i32,
) -> bool {
    if ptr.is_null() || w <= 0 || h <= 0 {
        return false;
    }

    let st = lock();
    let s = unsafe { surface_mut(ptr) };
    if s.surface.is_none() || !s.visible {
        return false;
    }
    if !st.use_gpu_paint {
        if st.present_mode == PresentMode::Drop {
            return false;
        }
        return queue_shm_present(s, &st, dirty, pixels, w, h);
    }
    if st.present_mode == PresentMode::Drop {
        return false;
    }

    let Some(ctx) = st.gpu_ctx.clone() else {
        tracing::error!("use_gpu_paint set but gpu_ctx missing");
        return false;
    };
    let Some(surface) = s.surface.as_ref() else {
        return false;
    };
    let raw_surface = surface.id().as_ptr() as *mut std::ffi::c_void;
    let Some(surface_ptr) = std::ptr::NonNull::new(raw_surface) else {
        return false;
    };

    s.buffer_w = w;
    s.buffer_h = h;
    set_viewport_for_buffer_locked(s, w, h);
    let painter_size = if s.pw > 0 && s.ph > 0 {
        (s.pw as u32, s.ph as u32)
    } else {
        (w as u32, h as u32)
    };

    if s.gpu_paint_worker.is_none() {
        s.gpu_paint_worker = Some(WaylandGpuPaintWorker::new(
            ctx,
            st.display_ptr,
            surface_ptr,
            painter_size,
            s.visible,
        ));
    }
    let Some(worker) = s.gpu_paint_worker.as_ref() else {
        return false;
    };
    worker.set_visible(s.visible);
    worker.resize(painter_size);
    let dirty = dirty
        .iter()
        .map(|r| DirtyRect {
            x: r.x,
            y: r.y,
            w: r.w,
            h: r.h,
        })
        .collect();
    worker.submit_frame(pixels, w as u32, h as u32, dirty);
    st.flush();
    true
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
    let Some(buf) = create_dmabuf_buffer(&st, frame.fd.as_fd(), frame.stride, frame.modifier, w, h)
    else {
        return;
    };
    if let Some(old) = s.popup_buffer.take() {
        old.destroy();
    }
    if let Some(vp) = s.popup_viewport.as_ref() {
        vp.set_source(0.0, 0.0, vw as f64, vh as f64);
        vp.set_destination(lw, lh);
    }
    let Some(popup) = s.popup_surface.as_ref() else {
        return;
    };
    popup.attach(Some(&buf), 0, 0);
    popup.damage_buffer(0, 0, vw, vh);
    // Commit parent first so subsurface state lands in the same frame.
    if let Some(parent) = s.surface.as_ref() {
        parent.commit();
    }
    popup.commit();
    st.flush();
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
        old.destroy();
    }
    if let Some(vp) = s.popup_viewport.as_ref() {
        vp.set_source(0.0, 0.0, pw as f64, ph as f64);
        vp.set_destination(lw, lh);
    }
    let Some(popup) = s.popup_surface.as_ref() else {
        return;
    };
    popup.attach(Some(&buf), 0, 0);
    popup.damage_buffer(0, 0, pw, ph);
    if let Some(parent) = s.surface.as_ref() {
        parent.commit();
    }
    popup.commit();
    st.flush();
    s.popup_buffer = Some(buf);
}

// =====================================================================
// Internal helpers
// =====================================================================

fn attach_and_commit_locked(
    s: &mut PlatformSurface,
    buf: wayland_client::protocol::wl_buffer::WlBuffer,
    w: i32,
    h: i32,
) {
    if let Some(old) = s.buffer.take() {
        old.destroy();
    }
    s.buffer_w = w;
    s.buffer_h = h;
    s.placeholder = false;
    s.null_attached = false;
    set_viewport_for_buffer_locked(s, w, h);
    let Some(surface) = s.surface.as_ref() else {
        return;
    };
    surface.attach(Some(&buf), 0, 0);
    surface.damage_buffer(0, 0, w, h);
    surface.commit();
    s.buffer = Some(buf);
}

fn set_viewport_for_buffer_locked(s: &mut PlatformSurface, w: i32, h: i32) {
    let Some(viewport) = s.viewport.as_ref() else {
        return;
    };
    if s.pw <= 0 || s.ph <= 0 || s.lw <= 0 || s.lh <= 0 {
        return;
    }
    if w > 0 && h > 0 {
        let src_w = w.min(s.pw);
        let src_h = h.min(s.ph);
        let dst_w = src_w * s.lw / s.pw;
        let dst_h = src_h * s.lh / s.ph;
        viewport.set_source(0.0, 0.0, src_w as f64, src_h as f64);
        viewport.set_destination(dst_w, dst_h);
    } else {
        viewport.set_destination(s.lw, s.lh);
    }
}

fn unmap_locked(s: &mut PlatformSurface) {
    let Some(surface) = s.surface.as_ref() else {
        return;
    };
    surface.attach(None, 0, 0);
    if let Some(viewport) = s.viewport.as_ref() {
        viewport.set_destination(-1, -1);
    }
    surface.commit();
    s.null_attached = true;
}

// =====================================================================
// Fullscreen transition
// =====================================================================

pub(crate) fn begin_transition() {
    let mut st = lock();
    begin_transition_locked(&mut st);
    st.flush();
}

pub(crate) fn end_transition() {
    let mut st = lock();
    end_transition_locked(&mut st);
    st.flush();
}

pub(crate) fn in_transition() -> bool {
    lock().transitioning
}

pub(crate) fn was_fullscreen() -> bool {
    lock().was_fullscreen
}

fn begin_transition_locked(st: &mut WlState) {
    st.transitioning = true;
    st.present_mode = PresentMode::Drop;
    let use_gpu_paint = st.use_gpu_paint;
    for &p in &st.stack {
        if p.is_null() {
            continue;
        }
        let s = unsafe { surface_mut(p) };
        let (Some(surface), Some(_)) = (s.surface.as_ref(), s.subsurface.as_ref()) else {
            continue;
        };
        // Vulkan WSI owns attach/commit on this wl_surface. Do not do wgpu
        // work here; only gate the presenter worker so queued frames do not
        // present during the transition.
        if use_gpu_paint {
            if let Some(worker) = s.gpu_paint_worker.as_ref() {
                worker.set_visible(false);
            }
            continue;
        }
        if let Some(worker) = s.shm_paint_worker.as_ref() {
            worker.set_visible(false);
        }
        surface.attach(None, 0, 0);
        if let Some(viewport) = s.viewport.as_ref() {
            viewport.set_destination(-1, -1);
        }
        surface.commit();
        s.null_attached = true;
    }
}

fn end_transition_locked(st: &mut WlState) {
    st.transitioning = false;
    st.present_mode = PresentMode::Attach;
    let use_gpu_paint = st.use_gpu_paint;
    if use_gpu_paint {
        // Reapply viewport state and re-enable presenter workers without
        // doing wgpu work on this callback.
        for &p in &st.stack {
            if p.is_null() {
                continue;
            }
            let s = unsafe { surface_mut(p) };
            set_viewport_for_buffer_locked(s, s.buffer_w, s.buffer_h);
            if let Some(worker) = s.gpu_paint_worker.as_ref() {
                worker.set_visible(s.visible);
                if s.pw > 0 && s.ph > 0 {
                    worker.resize((s.pw as u32, s.ph as u32));
                }
            }
        }
        return;
    }
    for &p in &st.stack {
        if p.is_null() {
            continue;
        }
        let s = unsafe { surface_mut(p) };
        if let Some(worker) = s.shm_paint_worker.as_ref() {
            worker.resize(s.lw, s.lh, s.pw, s.ph);
            worker.set_visible(s.visible);
        }
    }
    if let Some(&p) = st.stack.first()
        && !p.is_null()
    {
        let s = unsafe { surface_mut(p) };
        if let Some(viewport) = s.viewport.as_ref()
            && s.pw > 0
            && s.lw > 0
        {
            viewport.set_source(0.0, 0.0, s.pw as f64, s.ph as f64);
            viewport.set_destination(s.lw, s.lh);
        }
    }
}

pub(crate) fn on_configure(width: i32, height: i32, fullscreen: bool, cached_scale: f32) {
    if width <= 0 || height <= 0 {
        return;
    }
    let pw = width;
    let ph = height;
    let scale = if cached_scale > 0.0 {
        cached_scale
    } else {
        1.0
    };
    let lw = (pw as f32 / scale) as i32;
    let lh = (ph as f32 / scale) as i32;

    let mut st = lock();

    if fullscreen != st.was_fullscreen {
        if !st.transitioning {
            begin_transition_locked(&mut st);
        }
        st.was_fullscreen = fullscreen;
    }

    for &p in &st.stack {
        if p.is_null() {
            continue;
        }
        let s = unsafe { surface_mut(p) };
        s.lw = lw;
        s.lh = lh;
        s.pw = pw;
        s.ph = ph;
        if let Some(worker) = s.shm_paint_worker.as_ref() {
            worker.resize(lw, lh, pw, ph);
        }
    }

    update_surface_size_locked(&st, lw, lh, pw, ph);

    // pw now NEW. Flip paint gate back to Attach (keep transitioning=true).
    if st.transitioning {
        st.present_mode = PresentMode::Attach;
        if let Some(&p) = st.stack.first()
            && !p.is_null()
        {
            let s = unsafe { surface_mut(p) };
            if let Some(viewport) = s.viewport.as_ref()
                && s.pw > 0
                && s.lw > 0
            {
                viewport.set_source(0.0, 0.0, s.pw as f64, s.ph as f64);
                viewport.set_destination(s.lw, s.lh);
            }
        }
    }

    crate::wl_state::ensure_overlay_root_locked(&mut st);
    if let Some(vp) = st.overlay_vp.as_ref() {
        vp.set_destination(lw, lh);
        st.parent.commit();
    }
    st.flush();
}

fn update_surface_size_locked(st: &WlState, lw: i32, lh: i32, pw: i32, ph: i32) {
    if st.use_gpu_paint {
        let Some(&p) = st.stack.first() else {
            return;
        };
        if p.is_null() {
            return;
        }
        let s = unsafe { surface_mut(p) };
        set_viewport_for_buffer_locked(s, s.buffer_w, s.buffer_h);
        if let Some(worker) = s.gpu_paint_worker.as_ref() {
            worker.resize((pw.max(1) as u32, ph.max(1) as u32));
        }
        return;
    }
    let Some(&p) = st.stack.first() else {
        return;
    };
    if p.is_null() {
        return;
    }
    let s = unsafe { surface_mut(p) };
    if let Some(worker) = s.shm_paint_worker.as_ref() {
        worker.resize(lw, lh, pw, ph);
        return;
    }
    let (Some(surface), Some(viewport)) = (s.surface.as_ref(), s.viewport.as_ref()) else {
        return;
    };
    if st.transitioning {
        viewport.set_destination(lw, lh);
        surface.commit();
        return;
    }
    if s.buffer_w > 0 && s.buffer_h > 0 && pw > 0 && ph > 0 {
        let src_w = s.buffer_w.min(pw);
        let src_h = s.buffer_h.min(ph);
        let dst_w = src_w * lw / pw;
        let dst_h = src_h * lh / ph;
        viewport.set_source(0.0, 0.0, src_w as f64, src_h as f64);
        viewport.set_destination(dst_w, dst_h);
        surface.commit();
    }
}

pub(crate) fn set_fullscreen_via(fullscreen: bool, apply: impl FnOnce(bool)) {
    {
        let mut st = lock();
        if st.was_fullscreen == fullscreen {
            // Compositor may have rejected our previous toggle.
            if st.transitioning {
                end_transition_locked(&mut st);
                st.flush();
            }
            return;
        }
        begin_transition_locked(&mut st);
        st.flush();
    }
    apply(fullscreen);
}

pub(crate) fn toggle_fullscreen_via(apply: impl FnOnce(bool)) {
    let target = {
        let mut st = lock();
        begin_transition_locked(&mut st);
        st.flush();
        !st.was_fullscreen
    };
    apply(target);
}
