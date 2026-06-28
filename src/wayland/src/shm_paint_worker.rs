use std::os::fd::AsFd;
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::thread::{self, JoinHandle};

use memmap2::MmapOptions;
use wayland_client::protocol::{
    wl_buffer::WlBuffer,
    wl_shm::{Format, WlShm},
    wl_surface::WlSurface,
};
use wayland_client::{Connection, QueueHandle};
use wayland_protocols::wp::viewporter::client::wp_viewport::WpViewport;

use crate::wl_state::{DispatchState, memfd_anon};
use jfn_platform_abi::JfnRect;

struct PendingRect {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    pixels: Vec<u8>,
}

struct PendingFrame {
    rects: Vec<PendingRect>,
    full_pixels: Option<Vec<u8>>,
    width: i32,
    height: i32,
}

#[derive(Copy, Clone)]
pub(crate) struct ViewportState {
    pub(crate) lw: i32,
    pub(crate) lh: i32,
    pub(crate) pw: i32,
    pub(crate) ph: i32,
}

struct WorkerState {
    pending: Option<PendingFrame>,
    frame_size: (i32, i32),
    viewport: ViewportState,
    visible: bool,
    shutdown: bool,
}

/// Wayland wl_shm presenter.
///
/// CEF paint callbacks copy only dirty pixels into this worker and return.
/// Full-frame shadow maintenance, memfd/wl_shm buffer creation,
/// attach/commit, and flush all happen on the worker thread.
pub(crate) struct WaylandShmPaintWorker {
    shared: Arc<(Mutex<WorkerState>, Condvar)>,
    thread: Option<JoinHandle<()>>,
}

impl WaylandShmPaintWorker {
    pub(crate) fn new(
        conn: Connection,
        qh: QueueHandle<DispatchState>,
        shm: WlShm,
        surface: WlSurface,
        viewport: Option<WpViewport>,
        viewport_state: ViewportState,
        visible: bool,
    ) -> Self {
        let shared = Arc::new((
            Mutex::new(WorkerState {
                pending: None,
                frame_size: (0, 0),
                viewport: viewport_state,
                visible,
                shutdown: false,
            }),
            Condvar::new(),
        ));
        let worker_shared = Arc::clone(&shared);
        let thread = thread::spawn(move || {
            run_worker(conn, qh, shm, surface, viewport, worker_shared);
        });
        Self {
            shared,
            thread: Some(thread),
        }
    }

    pub(crate) fn resize(&self, lw: i32, lh: i32, pw: i32, ph: i32) {
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
        state.viewport = ViewportState { lw, lh, pw, ph };
        cv.notify_one();
    }

    pub(crate) fn set_visible(&self, visible: bool) {
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
        state.visible = visible;
        if !visible {
            state.pending = None;
        }
        cv.notify_one();
    }

    pub(crate) fn submit_frame(
        &self,
        pixels: &[u8],
        width: i32,
        height: i32,
        dirty: &[JfnRect],
    ) -> bool {
        if width <= 0 || height <= 0 {
            return false;
        }
        let stride = (width as usize).saturating_mul(4);
        let Some(len) = (height as usize).checked_mul(stride) else {
            return false;
        };
        if pixels.len() < len {
            return false;
        }

        let needs_full_copy = {
            let (lock, _) = &*self.shared;
            let state = lock.lock().unwrap_or_else(PoisonError::into_inner);
            state.frame_size != (width, height)
                || state
                    .pending
                    .as_ref()
                    .is_some_and(|frame| frame.full_pixels.is_some())
        };
        let full_pixels = if needs_full_copy {
            Some(pixels[..len].to_vec())
        } else {
            None
        };
        if dirty.is_empty() && full_pixels.is_none() {
            return true;
        }

        let mut rects = Vec::with_capacity(dirty.len());
        for rect in dirty {
            let Some(rect) = copy_dirty_rect(pixels.as_ptr(), stride, width, height, rect) else {
                continue;
            };
            rects.push(rect);
        }
        if rects.is_empty() && full_pixels.is_none() {
            return true;
        }

        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
        state.frame_size = (width, height);
        state.pending = Some(PendingFrame {
            rects,
            full_pixels,
            width,
            height,
        });
        cv.notify_one();
        true
    }

    pub(crate) fn shutdown(mut self) {
        let (lock, cv) = &*self.shared;
        {
            let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
            state.shutdown = true;
            state.pending = None;
            cv.notify_one();
        }
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn copy_dirty_rect(
    src: *const u8,
    src_stride: usize,
    width: i32,
    height: i32,
    rect: &JfnRect,
) -> Option<PendingRect> {
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
    if rx + rw > width {
        rw = width - rx;
    }
    if ry + rh > height {
        rh = height - ry;
    }
    if rw <= 0 || rh <= 0 {
        return None;
    }

    let row_bytes = (rw as usize) * 4;
    let mut pixels = Vec::with_capacity(row_bytes * rh as usize);
    for row in ry..(ry + rh) {
        let off = (row as usize) * src_stride + (rx as usize) * 4;
        let row = unsafe { std::slice::from_raw_parts(src.add(off), row_bytes) };
        pixels.extend_from_slice(row);
    }

    Some(PendingRect {
        x: rx,
        y: ry,
        w: rw,
        h: rh,
        pixels,
    })
}

fn run_worker(
    conn: Connection,
    qh: QueueHandle<DispatchState>,
    shm: WlShm,
    surface: WlSurface,
    viewport: Option<WpViewport>,
    shared: Arc<(Mutex<WorkerState>, Condvar)>,
) {
    let mut shadow = Vec::<u8>::new();
    let mut shadow_size = (0, 0);
    let mut current_buffer: Option<WlBuffer> = None;

    loop {
        let (frame, viewport_state, visible, shutdown) = {
            let (lock, cv) = &*shared;
            let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
            while state.pending.is_none() && !state.shutdown {
                state = cv.wait(state).unwrap_or_else(PoisonError::into_inner);
            }
            (
                state.pending.take(),
                state.viewport,
                state.visible,
                state.shutdown,
            )
        };

        if shutdown {
            break;
        }
        let Some(frame) = frame else {
            continue;
        };
        if !visible {
            continue;
        }

        if shadow_size != (frame.width, frame.height) {
            let stride = (frame.width as usize).saturating_mul(4);
            let Some(size) = (frame.height as usize).checked_mul(stride) else {
                continue;
            };
            shadow.clear();
            shadow.resize(size, 0);
            shadow_size = (frame.width, frame.height);
        }

        if let Some(full_pixels) = frame.full_pixels.as_ref() {
            shadow.copy_from_slice(full_pixels);
        }
        apply_dirty_to_shadow(&mut shadow, frame.width, &frame);
        let Some(buf) = create_shm_buffer(&shm, &qh, &shadow, frame.width, frame.height) else {
            tracing::warn!("wayland shm paint worker: buffer allocation failed");
            continue;
        };

        if let Some(old) = current_buffer.take() {
            crate::wl_state::retire_buffer(old);
        }
        set_viewport_for_buffer(viewport.as_ref(), viewport_state, frame.width, frame.height);
        surface.attach(Some(&buf), 0, 0);
        surface.damage_buffer(0, 0, frame.width, frame.height);
        surface.commit();
        let _ = conn.flush();
        // The layer commit caches this buffer (synchronized); the root-commit
        // owner applies it atomically with the window geometry.
        crate::root_window::request_present();
        current_buffer = Some(buf);
    }

    if let Some(buf) = current_buffer.take() {
        buf.destroy();
    }
    let _ = conn.flush();
}

fn apply_dirty_to_shadow(shadow: &mut [u8], width: i32, frame: &PendingFrame) {
    let dst_stride = (width as usize) * 4;
    for rect in &frame.rects {
        let row_bytes = (rect.w as usize) * 4;
        for row in 0..rect.h {
            let src_off = (row as usize) * row_bytes;
            let dst_off = ((rect.y + row) as usize) * dst_stride + (rect.x as usize) * 4;
            shadow[dst_off..dst_off + row_bytes]
                .copy_from_slice(&rect.pixels[src_off..src_off + row_bytes]);
        }
    }
}

fn create_shm_buffer(
    shm: &WlShm,
    qh: &QueueHandle<DispatchState>,
    pixels: &[u8],
    w: i32,
    h: i32,
) -> Option<WlBuffer> {
    let stride = w.checked_mul(4)?;
    let size = stride.checked_mul(h)?;
    if size <= 0 || pixels.len() < size as usize {
        return None;
    }
    let fd = memfd_anon("cef-sw-worker", size as usize)?;
    {
        let mut mmap = unsafe { MmapOptions::new().len(size as usize).map_mut(&fd) }.ok()?;
        mmap.copy_from_slice(&pixels[..size as usize]);
    }
    let pool = shm.create_pool(fd.as_fd(), size, qh, ());
    let buf = pool.create_buffer(0, w, h, stride, Format::Argb8888, qh, ());
    pool.destroy();
    Some(buf)
}

fn set_viewport_for_buffer(viewport: Option<&WpViewport>, state: ViewportState, w: i32, h: i32) {
    let Some(viewport) = viewport else {
        return;
    };
    if state.pw <= 0 || state.ph <= 0 || state.lw <= 0 || state.lh <= 0 {
        return;
    }
    if w > 0 && h > 0 {
        let src_w = w.min(state.pw);
        let src_h = h.min(state.ph);
        let dst_w = src_w * state.lw / state.pw;
        let dst_h = src_h * state.lh / state.ph;
        viewport.set_source(0.0, 0.0, src_w as f64, src_h as f64);
        viewport.set_destination(dst_w, dst_h);
    } else {
        viewport.set_destination(state.lw, state.lh);
    }
}
