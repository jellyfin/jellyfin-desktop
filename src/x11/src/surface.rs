//! Per-surface ops: alloc/free, software present, resize, visibility,
//! restack.
//!
//! # Safety
//!
//! `pub unsafe fn jfn_x11_*` entries take a `*mut PlatformSurface`
//! returned by [`jfn_x11_alloc_surface`]; callers must pass either that
//! handle or null, plus valid `JfnRect` / pixel-buffer pointers matching
//! the declared dimensions.

#![allow(clippy::missing_safety_doc)]

use std::ffi::{c_int, c_void};
use std::sync::Arc;

use jfn_gpu_paint::{DirtyRect, DmabufFrame, WindowTarget};
use x11rb::connection::Connection;
use x11rb::protocol::xproto::{
    AtomEnum, ConfigureWindowAux, ConnectionExt as _, CreateGCAux, CreateWindowAux, EventMask,
    PropMode, StackMode, WindowClass,
};
use x11rb::rust_connection::RustConnection;
use x11rb::wrapper::ConnectionExt as X11rbWrapperConnection;

use crate::x11_state::{MUT, Mutable, PlatformSurface, is_none_gc, is_none_window};

pub use jfn_platform_abi::JfnRect;

use jfn_playback::shutdown::jfn_shutting_down;

/// Create a WM-managed ARGB transient overlay window at (x, y, w, h).
/// Caller holds `MUT` and provides the mutable state borrow.
fn create_overlay_window(
    x11_conn: &RustConnection,
    m: &Mutable,
    x: i32,
    y: i32,
    w: u32,
    h: u32,
) -> u32 {
    let Ok(win_id) = x11_conn.generate_id() else {
        return 0;
    };
    let aux = CreateWindowAux::new()
        .background_pixel(0)
        .border_pixel(0)
        // Managed transient when windowed (the WM stacks/positions it without
        // flicker); unmanaged only when born into fullscreen, where the WM would
        // otherwise strut-clamp it below the panel.
        .override_redirect(u32::from(m.parent_fullscreen))
        .event_mask(EventMask::EXPOSURE)
        .colormap(m.colormap);
    let _ = x11_conn.create_window(
        m.argb_depth,
        win_id,
        m.root,
        x as i16,
        y as i16,
        w as u16,
        h as u16,
        0,
        WindowClass::INPUT_OUTPUT,
        m.argb_visual,
        &aux,
    );

    {
        // Tie the overlay to the app top-level so the WM raises/lowers/covers
        // them together. The overlay stays a separate top-level rather than a
        // child of it: sibling child windows are not composited against each
        // other on X11, so as a child its ARGB would not alpha-blend over the
        // video.
        let _ = x11_conn.change_property32(
            PropMode::REPLACE,
            win_id,
            u32::from(AtomEnum::WM_TRANSIENT_FOR),
            u32::from(AtomEnum::WINDOW),
            &[m.toplevel],
        );

        let _ = x11_conn.change_property32(
            PropMode::REPLACE,
            win_id,
            m.atoms.net_wm_window_type,
            u32::from(AtomEnum::ATOM),
            &[m.atoms.net_wm_window_type_normal],
        );
        let _ = x11_conn.change_property32(
            PropMode::REPLACE,
            win_id,
            m.atoms.net_wm_state,
            u32::from(AtomEnum::ATOM),
            &[
                m.atoms.net_wm_state_skip_taskbar,
                m.atoms.net_wm_state_skip_pager,
            ],
        );

        // Motif hints: flags=MWM_HINTS_DECORATIONS, decorations=0.
        let _ = x11_conn.change_property32(
            PropMode::REPLACE,
            win_id,
            m.atoms.motif_wm_hints,
            m.atoms.motif_wm_hints,
            &[2_u32, 0, 0, 0, 0],
        );

        // WM_HINTS: InputHint set, input=false; focus should stay on mpv.
        let _ = x11_conn.change_property32(
            PropMode::REPLACE,
            win_id,
            u32::from(AtomEnum::WM_HINTS),
            u32::from(AtomEnum::WM_HINTS),
            &[1_u32, 0, 0, 0, 0, 0, 0, 0, 0],
        );

        // No empty input shape — the overlay keeps a real input region so
        // GrabButton can capture clicks on it directly.

        // WM_DELETE_WINDOW handler.
        let _ = x11_conn.change_property32(
            PropMode::REPLACE,
            win_id,
            m.atoms.wm_protocols,
            u32::from(AtomEnum::ATOM),
            &[m.atoms.wm_delete_window],
        );
        let _ = x11_conn.flush();
    }

    win_id
}

pub fn jfn_x11_alloc_surface() -> *mut PlatformSurface {
    let s = Box::into_raw(Box::new(PlatformSurface::new()));
    let Some(x11_conn) = crate::x11_state::x11rb_conn() else {
        return s;
    };
    let mut g = MUT.lock();
    let Some(m) = g.as_mut() else {
        return s;
    };
    if is_none_window(m.parent) {
        return s;
    }

    let px = m.parent_x;
    let py = m.parent_y;
    let pw = if m.pw > 0 { m.pw as u32 } else { 1 };
    let ph = if m.ph > 0 { m.ph as u32 } else { 1 };

    let win = create_overlay_window(&x11_conn, m, px, py, pw, ph);
    // grab_overlay_input runs on a separate connection; round-trip first so the
    // window exists server-side, else its requests hit a silently-dropped
    // BadWindow and the overlay gets no input.
    if let Ok(cookie) = x11_conn.get_input_focus() {
        let _ = cookie.reply();
    }
    crate::input::grab_overlay_input(win);
    let gc = x11_conn.generate_id().unwrap_or(0);
    let _ = x11_conn.create_gc(gc, win, &CreateGCAux::new());

    unsafe {
        (*s).window = win;
        (*s).gc = gc;
        (*s).pw = pw as i32;
        (*s).ph = ph as i32;
        (*s).visible = true;
        (*s).fsm_state = Some(crate::overlay_fsm::OverlayState::new_mapped(
            m.parent_fullscreen,
        ));
    }
    let _ = x11_conn.map_window(win);
    // An override_redirect overlay (born fullscreen) is not stacked by the WM;
    // raise it above the app top-level ourselves.
    if m.parent_fullscreen {
        let aux = ConfigureWindowAux::new()
            .sibling(m.toplevel)
            .stack_mode(StackMode::ABOVE);
        let _ = x11_conn.configure_window(win, &aux);
    }
    let _ = x11_conn.flush();

    m.live.push(s);
    drop(g);

    // The parent may already be at its final WM placement with no further
    // ConfigureNotify coming, leaving this new overlay at stale geometry.
    crate::geometry::request_resync();
    s
}

pub unsafe fn jfn_x11_free_surface(s: *mut PlatformSurface) {
    if s.is_null() {
        return;
    }
    {
        let mut g = MUT.lock();
        if let Some(m) = g.as_mut()
            && let Some(pos) = m.live.iter().position(|&p| p == s)
        {
            // Order-preserving: m.live must stay in z-order for the geometry
            // watcher's restack. swap_remove would scramble it.
            m.live.remove(pos);
        }
    }

    let surf = unsafe { &mut *s };
    if let Some(worker) = surf.gpu_paint_worker.take() {
        worker.shutdown();
    }
    if let Some(worker) = surf.shm_paint_worker.take() {
        worker.shutdown();
    }
    if let Some(x11_conn) = crate::x11_state::x11rb_conn() {
        if !is_none_window(surf.window) {
            let _ = x11_conn.unmap_window(surf.window);
        }
        if !is_none_gc(surf.gc) {
            let _ = x11_conn.free_gc(surf.gc);
        }
        if !is_none_window(surf.window) {
            let _ = x11_conn.destroy_window(surf.window);
        }
        let _ = x11_conn.flush();
    }
    drop(unsafe { Box::from_raw(s) });
}

/// Present a CEF `OnAcceleratedPaint` dmabuf frame through the GPU worker. Only
/// reached when the dmabuf tier resolved at init (`use_dmabuf`). The caller has
/// already unpacked `CefAcceleratedPaintInfo` into `frame`.
pub unsafe fn jfn_x11_surface_present_dmabuf(s: *mut PlatformSurface, frame: DmabufFrame) -> bool {
    if jfn_shutting_down() || s.is_null() {
        return false;
    }
    let Some(conn_ptr) = crate::x11_state::raw_xcb_connection() else {
        return false;
    };
    let mut g = MUT.lock();
    let Some(m) = g.as_mut() else {
        return false;
    };

    let surf = unsafe { &mut *s };
    if is_none_window(surf.window) || !surf.visible {
        return false;
    }

    // Drop stale-size frames while a resize is in flight so the last good frame
    // holds until CEF relays out at the new size. Gate on the visible size; the
    // coded size can be padded.
    let gate_size = if frame.visible_w > 0 && frame.visible_h > 0 {
        (frame.visible_w as i32, frame.visible_h as i32)
    } else {
        (frame.width as i32, frame.height as i32)
    };
    if m.gate.main_present_decision(gate_size)
        == jfn_compositor_core::transition::PresentDecision::Reject
    {
        return false;
    }

    let Some(ctx) = m.gpu_ctx.clone() else {
        return false;
    };
    if surf
        .gpu_paint_worker
        .as_ref()
        .is_some_and(|worker| worker.failed())
    {
        return false;
    }

    if surf.gpu_paint_worker.is_none() {
        let target = WindowTarget::Xcb {
            connection: conn_ptr,
            window: surf.window,
            screen: m.screen_num,
            visual: m.argb_visual,
        };
        let size = (frame.width.max(1), frame.height.max(1));
        surf.gpu_paint_worker = Some(crate::gpu_paint_worker::X11GpuPaintWorker::new(
            ctx,
            target,
            size,
            surf.visible,
        ));
    }

    let Some(worker) = surf.gpu_paint_worker.as_ref() else {
        return false;
    };
    worker.set_visible(surf.visible);
    worker.submit_dmabuf(frame)
}

/// Lazily build the per-surface GPU presenter worker and queue `buffer`
/// through it. Returns false once the worker has failed so caller falls back
/// to SHM on subsequent frames.
#[allow(clippy::too_many_arguments)]
fn queue_gpu_present(
    surf: &mut PlatformSurface,
    m: &Mutable,
    dirty: *const JfnRect,
    dirty_len: usize,
    buffer: *const c_void,
    w: c_int,
    h: c_int,
) -> bool {
    let Some(ctx) = m.gpu_ctx.clone() else {
        return false;
    };
    let size = (w as u32, h as u32);

    if surf
        .gpu_paint_worker
        .as_ref()
        .is_some_and(|worker| worker.failed())
    {
        return false;
    }

    if surf.gpu_paint_worker.is_none() {
        let Some(conn_ptr) = crate::x11_state::raw_xcb_connection() else {
            return false;
        };
        let target = WindowTarget::Xcb {
            connection: conn_ptr,
            window: surf.window,
            screen: m.screen_num,
            visual: m.argb_visual,
        };
        surf.gpu_paint_worker = Some(crate::gpu_paint_worker::X11GpuPaintWorker::new(
            ctx,
            target,
            size,
            surf.visible,
        ));
    }

    let stride = (w as u32).saturating_mul(4);
    let Some(len) = (h as usize).checked_mul(stride as usize) else {
        return false;
    };
    let bgra = unsafe { std::slice::from_raw_parts(buffer as *const u8, len) };
    let dirty_rects = unsafe { std::slice::from_raw_parts(dirty, dirty_len) };
    let owned: Vec<DirtyRect> = dirty_rects
        .iter()
        .map(|r| DirtyRect {
            x: r.x,
            y: r.y,
            w: r.w,
            h: r.h,
        })
        .collect();

    let Some(worker) = surf.gpu_paint_worker.as_ref() else {
        return false;
    };
    worker.set_visible(surf.visible);
    worker.resize(size);
    worker.submit_frame(bgra, w as u32, h as u32, owned)
}

#[allow(clippy::too_many_arguments)]
fn queue_shm_present(
    surf: &mut PlatformSurface,
    m: &Mutable,
    conn: Arc<RustConnection>,
    dirty: *const JfnRect,
    dirty_len: usize,
    buffer: *const c_void,
    w: c_int,
    h: c_int,
) -> bool {
    if surf.shm_paint_worker.is_none() {
        surf.shm_paint_worker = Some(crate::shm_paint_worker::X11ShmPaintWorker::new(
            conn,
            surf.window,
            surf.gc,
            m.argb_depth,
            surf.visible,
        ));
    }

    let Some(worker) = surf.shm_paint_worker.as_ref() else {
        return false;
    };
    worker.set_visible(surf.visible);
    worker.submit_frame(buffer, w, h, dirty, dirty_len)
}

pub unsafe fn jfn_x11_surface_present_software(
    s: *mut PlatformSurface,
    dirty: *const JfnRect,
    dirty_len: usize,
    buffer: *const c_void,
    w: c_int,
    h: c_int,
) -> bool {
    if jfn_shutting_down() || s.is_null() || buffer.is_null() || w <= 0 || h <= 0 {
        return false;
    }
    let Some(x11_conn) = crate::x11_state::x11rb_conn() else {
        return false;
    };
    let mut g = MUT.lock();
    let Some(m) = g.as_mut() else {
        return false;
    };

    let surf = unsafe { &mut *s };
    if is_none_window(surf.window) || !surf.visible {
        return false;
    }

    // GPU pixel-upload path. Falls through to SHM on any failure so a
    // bad first frame doesn't strand the surface.
    if m.gpu_caps.gpu_available && queue_gpu_present(surf, m, dirty, dirty_len, buffer, w, h) {
        return true;
    }

    queue_shm_present(surf, m, x11_conn, dirty, dirty_len, buffer, w, h)
}

pub unsafe fn jfn_x11_surface_resize(
    s: *mut PlatformSurface,
    _lw: c_int,
    _lh: c_int,
    pw: c_int,
    ph: c_int,
) {
    if s.is_null() {
        return;
    }
    let Some(x11_conn) = crate::x11_state::x11rb_conn() else {
        return;
    };
    let mut g = MUT.lock();
    let Some(m) = g.as_mut() else { return };

    let old = (m.pw, m.ph);
    m.pw = pw;
    m.ph = ph;
    let surf = unsafe { &mut *s };
    surf.pw = pw;
    surf.ph = ph;

    // On the dmabuf tier the GPU worker sizes the overlay in lockstep with the
    // frame it presents, so don't drive the window size ahead of content here;
    // just arm the gate to drop stale-size frames during the resize.
    let dmabuf_lockstep = m.use_dmabuf && surf.gpu_paint_worker.is_some();
    if dmabuf_lockstep && pw > 0 && ph > 0 && old != (pw, ph) {
        m.gate.begin_capturing(old);
        m.gate.set_expected((pw, ph));
    }

    if pw > 0
        && ph > 0
        && let Some(worker) = surf.gpu_paint_worker.as_ref()
    {
        worker.resize((pw as u32, ph as u32));
    }
    if is_none_window(surf.window) {
        return;
    }

    // Size only: overlay position is owned exclusively by the geometry thread;
    // writing X/Y here would race it with a stale value during
    // fullscreen/maximize transitions.
    if !dmabuf_lockstep {
        let aux = ConfigureWindowAux::new().width(pw as u32).height(ph as u32);
        let _ = x11_conn.configure_window(surf.window, &aux);
        let _ = x11_conn.flush();
    }
}

pub unsafe fn jfn_x11_surface_set_visible(s: *mut PlatformSurface, visible: bool) {
    if s.is_null() {
        return;
    }
    let Some(x11_conn) = crate::x11_state::x11rb_conn() else {
        return;
    };
    let mut g = MUT.lock();
    let Some(m) = g.as_mut() else { return };

    let surf = unsafe { &mut *s };
    if surf.visible == visible {
        return;
    }
    surf.visible = visible;
    if is_none_window(surf.window) {
        return;
    }

    if visible {
        // Place from the geometry thread's cached parent geometry, the single
        // source of truth for position; re-querying here would race it.
        let pick = |s: i32, p: i32| -> u32 {
            if s > 0 {
                s as u32
            } else if p > 0 {
                p as u32
            } else {
                1
            }
        };
        let pw = pick(surf.pw, m.pw);
        let ph = pick(surf.ph, m.ph);
        let aux = ConfigureWindowAux::new()
            .x(m.parent_x)
            .y(m.parent_y)
            .width(pw)
            .height(ph);
        let _ = x11_conn.configure_window(surf.window, &aux);
        let _ = x11_conn.map_window(surf.window);
    } else {
        let _ = x11_conn.unmap_window(surf.window);
    }
    if let Some(worker) = surf.gpu_paint_worker.as_ref() {
        worker.set_visible(visible);
    }
    let _ = x11_conn.flush();
}

/// Stack `ordered[0..n]` above the app top-level, bottom to top.
pub unsafe fn jfn_x11_restack(ordered: *const *mut PlatformSurface, n: usize) {
    if n == 0 || ordered.is_null() {
        return;
    }
    let Some(x11_conn) = crate::x11_state::x11rb_conn() else {
        return;
    };
    let g = MUT.lock();
    let Some(m) = g.as_ref() else { return };

    let slice = unsafe { std::slice::from_raw_parts(ordered, n) };
    let mut prev = m.toplevel;
    for &s_ptr in slice {
        if s_ptr.is_null() {
            continue;
        }
        let s = unsafe { &*s_ptr };
        let window = s.window;
        if is_none_window(s.window) || window == prev {
            continue;
        }
        let aux = ConfigureWindowAux::new()
            .sibling(prev)
            .stack_mode(StackMode::ABOVE);
        let _ = x11_conn.configure_window(window, &aux);
        prev = window;
    }
    let _ = x11_conn.flush();
}
