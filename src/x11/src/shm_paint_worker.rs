use std::ffi::c_void;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

use xcb::x;

use crate::shm::{shm_alloc, shm_free};
use crate::surface::JfnRect;
use crate::x11_state::ShmBuffer;

struct PendingRect {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    pixels: Vec<u8>,
}

struct PendingFrame {
    rects: Vec<PendingRect>,
    width: i32,
    height: i32,
}

struct WorkerState {
    pending: Option<PendingFrame>,
    visible: bool,
    shutdown: bool,
}

/// X11 MIT-SHM presenter.
///
/// The CEF paint callback copies only the dirty pixels into this worker and
/// returns. SHM buffer allocation, expansion into the SHM segment,
/// xcb-shm PutImage requests, and flushes happen on the worker thread.
pub(crate) struct X11ShmPaintWorker {
    shared: Arc<(Mutex<WorkerState>, Condvar)>,
    thread: Option<JoinHandle<()>>,
}

impl X11ShmPaintWorker {
    pub(crate) fn new(
        conn: Arc<xcb::Connection>,
        window: x::Window,
        gc: x::Gcontext,
        depth: u8,
        visible: bool,
    ) -> Self {
        let shared = Arc::new((
            Mutex::new(WorkerState {
                pending: None,
                visible,
                shutdown: false,
            }),
            Condvar::new(),
        ));
        let worker_shared = Arc::clone(&shared);
        let thread = thread::spawn(move || {
            run_worker(conn, window, gc, depth, worker_shared);
        });
        Self {
            shared,
            thread: Some(thread),
        }
    }

    pub(crate) fn set_visible(&self, visible: bool) {
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap();
        state.visible = visible;
        if !visible {
            state.pending = None;
        }
        cv.notify_one();
    }

    pub(crate) fn submit_frame(
        &self,
        pixels: *const c_void,
        width: i32,
        height: i32,
        dirty: *const JfnRect,
        dirty_len: usize,
    ) -> bool {
        if pixels.is_null() || width <= 0 || height <= 0 || dirty_len == 0 {
            return false;
        }
        if dirty.is_null() {
            return false;
        }

        let stride = (width as usize).saturating_mul(4);
        let dirty = unsafe { std::slice::from_raw_parts(dirty, dirty_len) };
        let mut rects = Vec::with_capacity(dirty.len());
        for rect in dirty {
            let Some(rect) = copy_dirty_rect(pixels as *const u8, stride, width, height, rect)
            else {
                continue;
            };
            rects.push(rect);
        }
        if rects.is_empty() {
            return true;
        }

        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap();
        state.pending = Some(PendingFrame {
            rects,
            width,
            height,
        });
        cv.notify_one();
        true
    }

    pub(crate) fn shutdown(mut self) {
        let (lock, cv) = &*self.shared;
        {
            let mut state = lock.lock().unwrap();
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
    conn: Arc<xcb::Connection>,
    window: x::Window,
    gc: x::Gcontext,
    depth: u8,
    shared: Arc<(Mutex<WorkerState>, Condvar)>,
) {
    let mut bufs = [ShmBuffer::default(), ShmBuffer::default()];
    let mut buf_idx = 0usize;

    loop {
        let (frame, visible, shutdown) = {
            let (lock, cv) = &*shared;
            let mut state = lock.lock().unwrap();
            while state.pending.is_none() && !state.shutdown {
                state = cv.wait(state).unwrap();
            }
            (state.pending.take(), state.visible, state.shutdown)
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

        let buf = &mut bufs[buf_idx];
        if !shm_alloc(buf, &conn, frame.width, frame.height) {
            eprintln!("[x11] shm paint worker allocation failed");
            continue;
        }

        present_frame(&conn, window, gc, depth, buf, &frame);
        buf_idx ^= 1;
        let _ = conn.flush();
    }

    for buf in &mut bufs {
        shm_free(buf, Some(&conn));
    }
    let _ = conn.flush();
}

fn present_frame(
    conn: &xcb::Connection,
    window: x::Window,
    gc: x::Gcontext,
    depth: u8,
    buf: &mut ShmBuffer,
    frame: &PendingFrame,
) {
    let dst_stride = (frame.width as usize) * 4;
    for rect in &frame.rects {
        let row_bytes = (rect.w as usize) * 4;
        for row in 0..rect.h {
            let src_off = (row as usize) * row_bytes;
            let dst_off = ((rect.y + row) as usize) * dst_stride + (rect.x as usize) * 4;
            unsafe {
                std::ptr::copy_nonoverlapping(
                    rect.pixels.as_ptr().add(src_off),
                    buf.data.add(dst_off),
                    row_bytes,
                );
            }
        }
        conn.send_request(&xcb::shm::PutImage {
            drawable: x::Drawable::Window(window),
            gc,
            total_width: frame.width as u16,
            total_height: frame.height as u16,
            src_x: rect.x as u16,
            src_y: rect.y as u16,
            src_width: rect.w as u16,
            src_height: rect.h as u16,
            dst_x: rect.x as i16,
            dst_y: rect.y as i16,
            depth,
            format: x::ImageFormat::ZPixmap as u8,
            send_event: false,
            offset: 0,
            shmseg: buf.seg,
        });
    }
}
