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
use std::ptr;
use std::ptr::NonNull;

use jfn_gpu_paint::{DirtyRect, GpuPainter, PixelFrame, WindowTarget};
use xcb::{Xid, x};

use crate::lifecycle::query_parent_geometry;
use crate::shm::{shm_alloc, shm_free};
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
    for buf in &mut surf.bufs {
        shm_free(buf, Some(&conn));
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

/// Accelerated present is not supported on the X11 backend. v1 of
/// gpu_paint will route CEF dmabufs through here.
pub fn jfn_x11_surface_present(_s: *mut PlatformSurface, _info: *const c_void) -> bool {
    false
}

/// Lazily build the per-surface [`GpuPainter`] and present `buffer`
/// through it. Returns false on any failure; caller falls back to SHM.
#[allow(clippy::too_many_arguments)]
fn try_gpu_present(
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

    if surf.painter.is_none() {
        let Some(conn_ptr) = NonNull::new(conn.get_raw_conn() as *mut std::ffi::c_void) else {
            return false;
        };
        let target = WindowTarget::Xcb {
            connection: conn_ptr,
            window: surf.window.resource_id(),
            screen: m.screen_num,
            visual: m.argb_visual,
        };
        match GpuPainter::new(ctx, target, size) {
            Ok(p) => surf.painter = Some(p),
            Err(e) => {
                eprintln!("[x11] gpu_paint painter init failed: {e}; using SHM");
                return false;
            }
        }
    }
    let painter = surf.painter.as_mut().unwrap();
    painter.resize(size);

    let stride = (w as u32) * 4;
    let bgra = unsafe {
        std::slice::from_raw_parts(buffer as *const u8, (h as usize) * (stride as usize))
    };
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
    let frame = PixelFrame {
        width: w as u32,
        height: h as u32,
        stride,
        bgra,
        dirty: &owned,
    };
    match painter.push_pixels(frame) {
        Ok(()) => true,
        Err(e) => {
            eprintln!("[x11] gpu_paint push_pixels failed: {e}; using SHM");
            false
        }
    }
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
    if m.gpu_caps.gpu_available && try_gpu_present(surf, m, &conn, dirty, dirty_len, buffer, w, h) {
        return true;
    }

    let buf = &mut surf.bufs[surf.buf_idx];
    if !shm_alloc(buf, &conn, w, h) {
        return false;
    }

    let stride = (w as usize) * 4;
    let src = buffer as *const u8;
    let dirty_slice = unsafe { std::slice::from_raw_parts(dirty, dirty_len) };

    let depth = m.argb_depth;
    for rect in dirty_slice {
        let mut rx = rect.x;
        let mut ry = rect.y;
        let mut rw = rect.w;
        let mut rh = rect.h;
        if rx < 0 {
            rw += rx;
            rx = 0;
        }
        if ry < 0 {
            rh += ry;
            ry = 0;
        }
        if rx + rw > w {
            rw = w - rx;
        }
        if ry + rh > h {
            rh = h - ry;
        }
        if rw <= 0 || rh <= 0 {
            continue;
        }
        for row in ry..(ry + rh) {
            let off = (row as usize) * stride + (rx as usize) * 4;
            unsafe {
                ptr::copy_nonoverlapping(src.add(off), buf.data.add(off), (rw as usize) * 4);
            }
        }
        conn.send_request(&xcb::shm::PutImage {
            drawable: x::Drawable::Window(surf.window),
            gc: surf.gc,
            total_width: w as u16,
            total_height: h as u16,
            src_x: rx as u16,
            src_y: ry as u16,
            src_width: rw as u16,
            src_height: rh as u16,
            dst_x: rx as i16,
            dst_y: ry as i16,
            depth,
            format: x::ImageFormat::ZPixmap as u8,
            send_event: false,
            offset: 0,
            shmseg: buf.seg,
        });
    }

    surf.buf_idx ^= 1;
    let _ = conn.flush();
    true
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

    m.pw = pw;
    m.ph = ph;
    let surf = unsafe { &mut *s };
    surf.pw = pw;
    surf.ph = ph;
    if is_none_window(surf.window) {
        return;
    }

    // Refresh parent position too — fullscreen and inter-monitor moves
    // both arrive through this path.
    if let Some((px, py, _, _)) = query_parent_geometry(&conn, m.parent, m.root) {
        m.parent_x = px;
        m.parent_y = py;
    }

    conn.send_request(&x::ConfigureWindow {
        window: surf.window,
        value_list: &[
            x::ConfigWindow::X(m.parent_x),
            x::ConfigWindow::Y(m.parent_y),
            x::ConfigWindow::Width(pw as u32),
            x::ConfigWindow::Height(ph as u32),
        ],
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
