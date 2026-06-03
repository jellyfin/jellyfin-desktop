use std::ffi::{c_int, c_void};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

use jfn_gpu_paint::{DirtyRect, DmabufFrame, GpuContext, GpuPainter, PixelFrame, WindowTarget};

// Minimal libxcb FFI for sizing the overlay window from the worker thread.
// libxcb is thread-safe, so configuring on the shared connection here keeps
// the window extent in lockstep with the swapchain extent set by the present
// just below. The xcb crate links libxcb, so these resolve at link time.
const XCB_CONFIG_WINDOW_WIDTH: u16 = 1 << 2;
const XCB_CONFIG_WINDOW_HEIGHT: u16 = 1 << 3;

#[repr(C)]
struct XcbVoidCookie {
    sequence: std::ffi::c_uint,
}

unsafe extern "C" {
    fn xcb_configure_window(
        c: *mut c_void,
        window: u32,
        value_mask: u16,
        value_list: *const c_void,
    ) -> XcbVoidCookie;
    fn xcb_flush(c: *mut c_void) -> c_int;
}

/// Resize the overlay window to `(w, h)` and flush, via raw libxcb on the
/// worker thread. `conn` is the `xcb_connection_t*` copied out of the Xcb
/// target; `window` is the overlay XID.
unsafe fn configure_window_size(conn: *mut c_void, window: u32, w: u32, h: u32) {
    let value_mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    let value_list: [u32; 2] = [w, h];
    unsafe {
        xcb_configure_window(
            conn,
            window,
            value_mask,
            value_list.as_ptr() as *const c_void,
        );
        xcb_flush(conn);
    }
}

enum PendingFrame {
    Pixels {
        pixels: Vec<u8>,
        dirty: Vec<DirtyRect>,
        width: u32,
        height: u32,
        stride: u32,
    },
    Dmabuf(DmabufFrame),
}

impl PendingFrame {
    fn size(&self) -> (u32, u32) {
        match self {
            PendingFrame::Pixels { width, height, .. } => (*width, *height),
            PendingFrame::Dmabuf(f) => (f.width, f.height),
        }
    }
}

struct WorkerState {
    pending: Option<PendingFrame>,
    target_size: (u32, u32),
    visible: bool,
    shutdown: bool,
}

/// X11 GPU pixel-upload presenter.
///
/// The CEF paint callback only copies the latest frame into this worker and
/// returns. Vulkan surface creation/acquire/upload/present all happen on the
/// worker thread so paint is not blocked on wgpu. If the GPU path fails, the
/// worker is permanently marked failed and callers fall back to MIT-SHM on
/// subsequent frames.
pub(crate) struct X11GpuPaintWorker {
    shared: Arc<(Mutex<WorkerState>, Condvar)>,
    failed: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl X11GpuPaintWorker {
    pub(crate) fn new(
        ctx: Arc<GpuContext>,
        target: WindowTarget,
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
        let failed = Arc::new(AtomicBool::new(false));
        let worker_shared = Arc::clone(&shared);
        let worker_failed = Arc::clone(&failed);
        let thread = thread::spawn(move || {
            run_worker(ctx, target, worker_shared, worker_failed);
        });
        Self {
            shared,
            failed,
            thread: Some(thread),
        }
    }

    pub(crate) fn failed(&self) -> bool {
        self.failed.load(Ordering::Acquire)
    }

    pub(crate) fn resize(&self, size: (u32, u32)) {
        if size.0 == 0 || size.1 == 0 {
            return;
        }
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap();
        state.target_size = size;
        cv.notify_one();
    }

    pub(crate) fn set_visible(&self, visible: bool) {
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap();
        state.visible = visible;
        cv.notify_one();
    }

    pub(crate) fn submit_frame(
        &self,
        pixels: &[u8],
        width: u32,
        height: u32,
        dirty: Vec<DirtyRect>,
    ) -> bool {
        if self.failed() {
            return false;
        }
        let stride = width.saturating_mul(4);
        let Some(len) = (height as usize).checked_mul(stride as usize) else {
            return false;
        };
        if pixels.len() < len {
            return false;
        }
        let frame = PendingFrame::Pixels {
            pixels: pixels[..len].to_vec(),
            dirty,
            width,
            height,
            stride,
        };
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap();
        // Latest-frame only: replace any frame the presenter has not consumed.
        state.pending = Some(frame);
        cv.notify_one();
        true
    }

    pub(crate) fn submit_dmabuf(&self, frame: DmabufFrame) -> bool {
        if self.failed() {
            return false;
        }
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap();
        // Latest-frame only: a superseded dmabuf frame drops here, closing
        // its fds.
        state.pending = Some(PendingFrame::Dmabuf(frame));
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

fn run_worker(
    ctx: Arc<GpuContext>,
    target: WindowTarget,
    shared: Arc<(Mutex<WorkerState>, Condvar)>,
    failed: Arc<AtomicBool>,
) {
    let mut painter: Option<GpuPainter> = None;

    // Raw handles for lockstep window sizing on the dmabuf path. Copied out
    // before `target` is consumed into the painter below.
    let (xcb_conn, xcb_window) = match &target {
        WindowTarget::Xcb {
            connection, window, ..
        } => (connection.as_ptr(), Some(*window)),
        _ => (std::ptr::null_mut(), None),
    };
    let mut last_configured: Option<(u32, u32)> = None;

    let mut target = Some(target);

    loop {
        let (frame, visible, target_size, shutdown) = {
            let (lock, cv) = &*shared;
            let mut state = lock.lock().unwrap();
            while state.pending.is_none() && !state.shutdown {
                state = cv.wait(state).unwrap();
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
            let Some(target) = target.take() else {
                failed.store(true, Ordering::Release);
                break;
            };
            match GpuPainter::new(ctx.clone(), target, frame.size()) {
                Ok(p) => painter = Some(p),
                Err(e) => {
                    eprintln!("[x11] gpu_paint worker init failed: {e}; using SHM");
                    failed.store(true, Ordering::Release);
                    break;
                }
            }
        }

        let painter = painter.as_mut().unwrap();
        painter.set_visible(visible);
        painter.resize(target_size);
        match frame {
            PendingFrame::Pixels {
                pixels,
                dirty,
                width,
                height,
                stride,
            } => {
                let pixel_frame = PixelFrame {
                    width,
                    height,
                    stride,
                    bgra: &pixels,
                    dirty: &dirty,
                };
                if let Err(e) = painter.push_pixels(pixel_frame) {
                    eprintln!("[x11] gpu_paint worker push_pixels failed: {e}; using SHM");
                    failed.store(true, Ordering::Release);
                    break;
                }
            }
            PendingFrame::Dmabuf(dmabuf) => {
                // Lockstep: size the overlay to exactly this frame before the
                // swapchain reconfigures to it on present, so window extent ==
                // swapchain extent == content. jfn_x11_surface_resize omits the
                // window size for the dmabuf tier, deferring it to here.
                if let Some(window) = xcb_window {
                    let size = (dmabuf.width, dmabuf.height);
                    if last_configured != Some(size) {
                        unsafe { configure_window_size(xcb_conn, window, size.0, size.1) };
                        last_configured = Some(size);
                    }
                }
                // Transient failure: drop this frame and keep going. CEF is
                // producing dmabufs (shared textures on), so there is no
                // per-surface CPU fallback — latching failed would strand it.
                if let Err(e) = painter.push_dmabuf(dmabuf) {
                    tracing::warn!("[x11] gpu_paint worker push_dmabuf failed: {e}");
                }
            }
        }
    }

    if let Some(painter) = painter {
        painter.shutdown();
    }
}
