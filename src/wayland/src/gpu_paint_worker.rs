use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::thread::{self, JoinHandle};

use jfn_gpu_paint::{DirtyRect, GpuContext, GpuPainter, PixelFrame, WindowTarget};

struct PendingFrame {
    pixels: Vec<u8>,
    dirty: Vec<DirtyRect>,
    width: u32,
    height: u32,
    stride: u32,
}

struct WorkerState {
    pending: Option<PendingFrame>,
    target_size: (u32, u32),
    visible: bool,
    shutdown: bool,
}

pub(crate) struct WaylandGpuPaintWorker {
    shared: Arc<(Mutex<WorkerState>, Condvar)>,
    thread: Option<JoinHandle<()>>,
}

impl WaylandGpuPaintWorker {
    pub(crate) fn new(
        ctx: Arc<GpuContext>,
        display_ptr: NonNull<c_void>,
        surface_ptr: NonNull<c_void>,
        size: (u32, u32),
        visible: bool,
    ) -> Self {
        let shared = Arc::new((
            Mutex::new(WorkerState {
                pending: None,
                target_size: size,
                visible,
                shutdown: false,
            }),
            Condvar::new(),
        ));
        let worker_shared = Arc::clone(&shared);
        let display_raw = display_ptr.as_ptr() as usize;
        let surface_raw = surface_ptr.as_ptr() as usize;
        let thread = thread::spawn(move || {
            run_worker(ctx, display_raw, surface_raw, worker_shared);
        });
        Self {
            shared,
            thread: Some(thread),
        }
    }

    pub(crate) fn resize(&self, size: (u32, u32)) {
        if size.0 == 0 || size.1 == 0 {
            return;
        }
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
        state.target_size = size;
        cv.notify_one();
    }

    pub(crate) fn set_visible(&self, visible: bool) {
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
        state.visible = visible;
        cv.notify_one();
    }

    pub(crate) fn submit_frame(
        &self,
        pixels: &[u8],
        width: u32,
        height: u32,
        dirty: Vec<DirtyRect>,
    ) {
        let stride = width.saturating_mul(4);
        let Some(len) = (height as usize).checked_mul(stride as usize) else {
            return;
        };
        if pixels.len() < len {
            return;
        }
        let frame = PendingFrame {
            pixels: pixels[..len].to_vec(),
            dirty,
            width,
            height,
            stride,
        };
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
        // Latest-frame only: replace any frame the presenter has not consumed.
        state.pending = Some(frame);
        cv.notify_one();
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

fn run_worker(
    ctx: Arc<GpuContext>,
    display_raw: usize,
    surface_raw: usize,
    shared: Arc<(Mutex<WorkerState>, Condvar)>,
) {
    let Some(display_ptr) = NonNull::new(display_raw as *mut c_void) else {
        return;
    };
    let Some(surface_ptr) = NonNull::new(surface_raw as *mut c_void) else {
        return;
    };
    let mut painter: Option<GpuPainter> = None;

    loop {
        let (frame, visible, target_size, shutdown) = {
            let (lock, cv) = &*shared;
            let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
            while state.pending.is_none() && !state.shutdown {
                state = cv.wait(state).unwrap_or_else(PoisonError::into_inner);
            }
            (
                state.pending.take(),
                state.visible,
                state.target_size,
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

        if painter.is_none() {
            let target = WindowTarget::Wayland {
                display: display_ptr,
                surface: surface_ptr,
            };
            match GpuPainter::new(ctx.clone(), target, (frame.width, frame.height)) {
                Ok(p) => painter = Some(p),
                Err(_) => continue,
            }
        }

        let Some(painter) = painter.as_mut() else {
            continue;
        };
        painter.set_visible(visible);
        painter.resize(target_size);
        let pixel_frame = PixelFrame {
            width: frame.width,
            height: frame.height,
            stride: frame.stride,
            bgra: &frame.pixels,
            dirty: &frame.dirty,
        };
        // If acquire/present is busy or transiently unavailable, drop this
        // frame. The compositor keeps showing the last presented image.
        let _ = painter.push_pixels(pixel_frame);
    }

    if let Some(painter) = painter {
        painter.shutdown();
    }
}
