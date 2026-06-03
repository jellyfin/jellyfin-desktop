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
use std::ptr::NonNull;
use std::sync::Arc;

use jfn_gpu_paint::{DirtyRect, DmabufFrame, WindowTarget};
use xcb::{Xid, x};

use crate::lifecycle::query_parent_geometry;
use crate::x11_state::{MUT, Mutable, PlatformSurface, is_none_gc, is_none_window};

pub use jfn_platform_abi::JfnRect;

use jfn_playback::shutdown::jfn_shutting_down;

/// Create an ARGB override-redirect overlay window at (x, y, w, h).
/// Caller holds `MUT` and provides the mutable state borrow.
fn create_overlay_window(
    conn: &xcb::Connection,
    m: &Mutable,
    x: i32,
    y: i32,
    w: u32,
    h: u32,
) -> x::Window {
    let win: x::Window = conn.generate_id();
    conn.send_request(&x::CreateWindow {
        depth: m.argb_depth,
        wid: win,
        parent: m.root,
        x: x as i16,
        y: y as i16,
        width: w as u16,
        height: h as u16,
        border_width: 0,
        class: x::WindowClass::InputOutput,
        visual: m.argb_visual,
        value_list: &[
            x::Cw::BackPixel(0),
            x::Cw::BorderPixel(0),
            x::Cw::OverrideRedirect(true),
            x::Cw::Colormap(m.colormap),
        ],
    });

    // Input-passthrough: empty input shape sends all input to mpv parent.
    conn.send_request(&xcb::shape::Rectangles {
        operation: xcb::shape::So::Set,
        destination_kind: xcb::shape::Sk::Input,
        ordering: x::ClipOrdering::Unsorted,
        destination_window: win,
        x_offset: 0,
        y_offset: 0,
        rectangles: &[],
    });

    // WM_DELETE_WINDOW handler.
    conn.send_request(&x::ChangeProperty {
        mode: x::PropMode::Replace,
        window: win,
        property: m.atoms.wm_protocols,
        r#type: x::ATOM_ATOM,
        data: &[m.atoms.wm_delete_window],
    });

    win
}

pub fn jfn_x11_alloc_surface() -> *mut PlatformSurface {
    let s = Box::into_raw(Box::new(PlatformSurface::new()));
    let Some(conn) = crate::x11_state::conn() else {
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

    let win = create_overlay_window(&conn, m, px, py, pw, ph);
    let gc: x::Gcontext = conn.generate_id();
    conn.send_request(&x::CreateGc {
        cid: gc,
        drawable: x::Drawable::Window(win),
        value_list: &[],
    });

    unsafe {
        (*s).window = win;
        (*s).gc = gc;
        (*s).pw = pw as i32;
        (*s).ph = ph as i32;
        (*s).visible = true;
    }
    conn.send_request(&x::MapWindow { window: win });
    let _ = conn.flush();

    m.live.push(s);
    s
}

pub unsafe fn jfn_x11_free_surface(s: *mut PlatformSurface) {
    if s.is_null() {
        return;
    }
    let Some(conn) = crate::x11_state::conn() else {
        // Connection gone; just drop the box.
        drop(unsafe { Box::from_raw(s) });
        return;
    };
    {
        let mut g = MUT.lock();
        if let Some(m) = g.as_mut()
            && let Some(pos) = m.live.iter().position(|&p| p == s)
        {
            m.live.swap_remove(pos);
        }
    }

    let surf = unsafe { &mut *s };
    if let Some(worker) = surf.gpu_paint_worker.take() {
        worker.shutdown();
    }
    if let Some(worker) = surf.shm_paint_worker.take() {
        worker.shutdown();
    }
    if !is_none_window(surf.window) {
        conn.send_request(&x::UnmapWindow {
            window: surf.window,
        });
    }
    if !is_none_gc(surf.gc) {
        conn.send_request(&x::FreeGc { gc: surf.gc });
    }
    if !is_none_window(surf.window) {
        conn.send_request(&x::DestroyWindow {
            window: surf.window,
        });
    }
    let _ = conn.flush();
    drop(unsafe { Box::from_raw(s) });
}

/// Present a CEF `OnAcceleratedPaint` dmabuf frame through the GPU
/// worker. Active only when the dmabuf tier resolved at init
/// (`use_dmabuf`); CEF then emits accelerated paints that route here
/// instead of the pixel-upload software path. The caller (make_platform)
/// has already unpacked `CefAcceleratedPaintInfo` into `frame`.
pub unsafe fn jfn_x11_surface_present_dmabuf(s: *mut PlatformSurface, frame: DmabufFrame) -> bool {
    if jfn_shutting_down() || s.is_null() {
        return false;
    }
    let Some(conn) = crate::x11_state::conn() else {
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

    // Drop stale-size frames while a resize is in flight so the last good
    // frame holds until CEF relays out at the new size (mirrors Windows'
    // main-surface present path). Compare the visible size; the coded size
    // can be padded.
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
        let Some(conn_ptr) = NonNull::new(conn.get_raw_conn() as *mut c_void) else {
            return false;
        };
        let target = WindowTarget::Xcb {
            connection: conn_ptr,
            window: surf.window.resource_id(),
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

    let worker = surf.gpu_paint_worker.as_ref().unwrap();
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
    conn: &xcb::Connection,
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
        let Some(conn_ptr) = NonNull::new(conn.get_raw_conn() as *mut std::ffi::c_void) else {
            return false;
        };
        let target = WindowTarget::Xcb {
            connection: conn_ptr,
            window: surf.window.resource_id(),
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

    let worker = surf.gpu_paint_worker.as_ref().unwrap();
    worker.set_visible(surf.visible);
    worker.resize(size);
    worker.submit_frame(bgra, w as u32, h as u32, owned)
}

#[allow(clippy::too_many_arguments)]
fn queue_shm_present(
    surf: &mut PlatformSurface,
    m: &Mutable,
    conn: Arc<xcb::Connection>,
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

    let worker = surf.shm_paint_worker.as_ref().unwrap();
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
    let Some(conn) = crate::x11_state::conn() else {
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
    if m.gpu_caps.gpu_available && queue_gpu_present(surf, m, &conn, dirty, dirty_len, buffer, w, h)
    {
        return true;
    }

    queue_shm_present(surf, m, conn, dirty, dirty_len, buffer, w, h)
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
    let Some(conn) = crate::x11_state::conn() else {
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

    // Dmabuf tier: the GPU worker sizes the overlay in lockstep with the
    // frame it presents (below), so don't drive the window size ahead of
    // content here. Arm the gate to drop stale-size frames during the
    // resize. Software tiers keep eager resize.
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

    // Refresh parent position too — fullscreen and inter-monitor moves
    // both arrive through this path.
    if let Some((px, py, _, _)) = query_parent_geometry(&conn, m.parent, m.root) {
        m.parent_x = px;
        m.parent_y = py;
    }

    let mut value_list = vec![
        x::ConfigWindow::X(m.parent_x),
        x::ConfigWindow::Y(m.parent_y),
    ];
    if !dmabuf_lockstep {
        value_list.push(x::ConfigWindow::Width(pw as u32));
        value_list.push(x::ConfigWindow::Height(ph as u32));
    }
    conn.send_request(&x::ConfigureWindow {
        window: surf.window,
        value_list: &value_list,
    });
    let _ = conn.flush();
}

pub unsafe fn jfn_x11_surface_set_visible(s: *mut PlatformSurface, visible: bool) {
    if s.is_null() {
        return;
    }
    let Some(conn) = crate::x11_state::conn() else {
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
        // Reposition to current parent geometry before mapping.
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
        conn.send_request(&x::ConfigureWindow {
            window: surf.window,
            value_list: &[
                x::ConfigWindow::X(m.parent_x),
                x::ConfigWindow::Y(m.parent_y),
                x::ConfigWindow::Width(pw),
                x::ConfigWindow::Height(ph),
            ],
        });
        conn.send_request(&x::MapWindow {
            window: surf.window,
        });
    } else {
        conn.send_request(&x::UnmapWindow {
            window: surf.window,
        });
    }
    if let Some(worker) = surf.gpu_paint_worker.as_ref() {
        worker.set_visible(visible);
    }
    let _ = conn.flush();
}

/// Stack `ordered[0..n]` above the mpv parent, bottom to top.
pub unsafe fn jfn_x11_restack(ordered: *const *mut PlatformSurface, n: usize) {
    if n == 0 || ordered.is_null() {
        return;
    }
    let Some(conn) = crate::x11_state::conn() else {
        return;
    };
    let g = MUT.lock();
    let Some(m) = g.as_ref() else { return };

    let slice = unsafe { std::slice::from_raw_parts(ordered, n) };
    let mut prev: x::Window = m.parent;
    for &s_ptr in slice {
        if s_ptr.is_null() {
            continue;
        }
        let s = unsafe { &*s_ptr };
        if is_none_window(s.window) {
            continue;
        }
        conn.send_request(&x::ConfigureWindow {
            window: s.window,
            value_list: &[
                x::ConfigWindow::Sibling(prev),
                x::ConfigWindow::StackMode(x::StackMode::Above),
            ],
        });
        prev = s.window;
    }
    let _ = conn.flush();
}
