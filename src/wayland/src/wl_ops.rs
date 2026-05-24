//! Surface lifecycle + paint + transition ops.
//!
//! All entry points run under the [`wl_state::lock()`] mutex. Each
//! protocol-touching op calls `WlState::flush()` (or `conn.flush()`)
//! before returning so commits land in compositor order matching the
//! C++ original.

use std::os::fd::BorrowedFd;

use wayland_client::protocol::wl_subsurface::WlSubsurface;

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
    let st = lock();
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
    let alpha = st
        .alpha_modifier
        .as_ref()
        .map(|am| am.get_surface(&surface, &st.qh, ()));

    surface.commit();
    st.flush();

    s.surface = Some(surface);
    s.subsurface = Some(subsurface);
    s.viewport = viewport;
    s.alpha = alpha;

    ptr
}

pub(crate) fn free_surface(ptr: *mut PlatformSurface) {
    if ptr.is_null() {
        return;
    }
    {
        let mut st = lock();
        // Drop from stack if still present.
        st.stack.retain(|p| *p != ptr);

        // SAFETY: stack drop above guarantees no aliases via stack;
        // caller (C++) guarantees no concurrent use of `ptr`.
        let s = unsafe { surface_mut(ptr) };
        popup_destroy_locked(s);
        if let Some(a) = s.alpha.take() {
            a.destroy();
        }
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
    let mut prev: &wayland_client::protocol::wl_surface::WlSurface = &st.parent;
    for &p in ordered {
        if p.is_null() {
            continue;
        }
        // SAFETY: stack pointers are valid for the lifetime of this call;
        // surface_mut borrows disjoint heap allocations.
        let s = unsafe { surface_mut(p) };
        let (Some(sub), Some(surf)) = (s.subsurface.as_ref(), s.surface.as_ref()) else {
            continue;
        };
        sub.place_above(prev);
        prev = surf;
    }
    st.flush();
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
        if let Some(alpha) = s.alpha.as_ref() {
            alpha.set_multiplier(u32::MAX);
        }
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

/// True when the surface has a `wp_alpha_modifier_surface_v1` proxy,
/// i.e. the compositor advertised `wp_alpha_modifier_v1` at bind time.
/// Used by the fade thunk to decide between animated fade and a hard
/// unmap (compositors without the protocol, e.g. niri, have no way to
/// drive per-surface opacity).
pub(crate) fn surface_has_alpha(ptr: *mut PlatformSurface) -> bool {
    if ptr.is_null() {
        return false;
    }
    let _st = lock();
    let s = unsafe { surface_mut(ptr) };
    s.alpha.is_some()
}

// =====================================================================
// Fade per-frame apply (called from Rust fade thread via FFI thunk)
// =====================================================================

/// Fade-frame helper: apply alpha multiplier under the WlState lock.
/// Returns false to abort the loop if surface state is gone.
pub(crate) fn fade_apply_frame(ptr: *mut PlatformSurface, alpha: u32) -> bool {
    if ptr.is_null() {
        return false;
    }
    let st = lock();
    let s = unsafe { surface_mut(ptr) };
    if !s.visible || s.surface.is_none() || s.alpha.is_none() {
        return false;
    }
    let surface = s.surface.as_ref().unwrap();
    let alpha_proxy = s.alpha.as_ref().unwrap();
    alpha_proxy.set_multiplier(alpha);
    surface.commit();
    st.flush();
    true
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

/// POD frame info — what the caller unpacks from CefAcceleratedPaintInfo.
#[repr(C)]
pub struct JfnDmabufFrame {
    pub fd: i32,
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

    let buf = {
        let fd = unsafe { BorrowedFd::borrow_raw(frame.fd) };
        create_dmabuf_buffer(&st, fd, frame.stride, frame.modifier, w, h)
    };
    let Some(buf) = buf else {
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

pub(crate) fn surface_present_software(
    ptr: *mut PlatformSurface,
    pixels: &[u8],
    w: i32,
    h: i32,
) -> bool {
    if ptr.is_null() {
        return false;
    }
    let st = lock();
    let s = unsafe { surface_mut(ptr) };
    if s.surface.is_none() || !s.visible {
        return false;
    }
    let Some(buf) = create_shm_buffer(&st, pixels, w, h) else {
        return false;
    };
    attach_and_commit_locked(s, buf, w, h);
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
    let buf = {
        let fd = unsafe { BorrowedFd::borrow_raw(frame.fd) };
        create_dmabuf_buffer(&st, fd, frame.stride, frame.modifier, w, h)
    };
    let Some(buf) = buf else {
        return;
    };
    if let Some(old) = s.popup_buffer.take() {
        old.destroy();
    }
    if let Some(vp) = s.popup_viewport.as_ref() {
        vp.set_source(0.0, 0.0, vw as f64, vh as f64);
        vp.set_destination(lw, lh);
    }
    let popup = s.popup_surface.as_ref().unwrap();
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
    let popup = s.popup_surface.as_ref().unwrap();
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
    if let Some(viewport) = s.viewport.as_ref()
        && s.pw > 0
        && s.lw > 0
    {
        let src_w = w.min(s.pw);
        let src_h = h.min(s.ph);
        let dst_w = src_w * s.lw / s.pw;
        let dst_h = src_h * s.lh / s.ph;
        viewport.set_source(0.0, 0.0, src_w as f64, src_h as f64);
        viewport.set_destination(dst_w, dst_h);
    }
    let surface = s.surface.as_ref().expect("attach without surface");
    surface.attach(Some(&buf), 0, 0);
    surface.damage_buffer(0, 0, w, h);
    surface.commit();
    s.buffer = Some(buf);
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
    for &p in &st.stack {
        if p.is_null() {
            continue;
        }
        let s = unsafe { surface_mut(p) };
        let (Some(surface), Some(_)) = (s.surface.as_ref(), s.subsurface.as_ref()) else {
            continue;
        };
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
    if let Some(&p) = st.stack.first() {
        if p.is_null() {
            return;
        }
        let s = unsafe { surface_mut(p) };
        if let (Some(viewport), pw, ph, lw, lh) = (s.viewport.as_ref(), s.pw, s.ph, s.lw, s.lh)
            && pw > 0
            && lw > 0
        {
            viewport.set_source(0.0, 0.0, pw as f64, ph as f64);
            viewport.set_destination(lw, lh);
        }
        let _ = (s,);
    }
}

// =====================================================================
// mpv-configure callback (called from C++ on_proxy_configure → FFI thunk)
// =====================================================================

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
    }

    update_surface_size_locked(&st, lw, lh, pw, ph);

    // pw now NEW. Flip paint gate back to Attach (keep transitioning=true).
    if st.transitioning {
        st.present_mode = PresentMode::Attach;
        if let Some(&p) = st.stack.first()
            && !p.is_null()
        {
            let s = unsafe { surface_mut(p) };
            if let (Some(viewport), true) = (s.viewport.as_ref(), s.pw > 0 && s.lw > 0) {
                viewport.set_source(0.0, 0.0, s.pw as f64, s.ph as f64);
                viewport.set_destination(s.lw, s.lh);
            }
        }
    }
    st.flush();
}

fn update_surface_size_locked(st: &WlState, lw: i32, lh: i32, pw: i32, ph: i32) {
    let Some(&p) = st.stack.first() else {
        return;
    };
    if p.is_null() {
        return;
    }
    let s = unsafe { surface_mut(p) };
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

pub(crate) fn set_fullscreen_via(fullscreen: bool, set_wlproxy: unsafe extern "C" fn(i32)) {
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
    unsafe { set_wlproxy(if fullscreen { 1 } else { 0 }) };
}

pub(crate) fn toggle_fullscreen_via(set_wlproxy: unsafe extern "C" fn(i32)) {
    let target = {
        let mut st = lock();
        begin_transition_locked(&mut st);
        st.flush();
        if st.was_fullscreen { 0 } else { 1 }
    };
    unsafe { set_wlproxy(target) };
}
