use std::os::fd::AsFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::thread::{self, JoinHandle};

use wayland_client::QueueHandle;
use wayland_client::protocol::wl_shm::WlShm;
use wayland_client::protocol::wl_surface::WlSurface;
use wayland_protocols::wp::linux_dmabuf::zv1::client::zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1;

use jfn_gpu_paint::{DirtyRect, GpuContext, GpuPainter, PixelFrame};
use jfn_platform_abi::JfnRect;

use crate::layer::{FrameCommit, LayerSurface, Present, PresentError, ViewportState};
use crate::wl_ops::JfnDmabufFrame;
use crate::wl_state::{
    DispatchState, DmabufBuf, OwnedBuffer, buffer_is_idle, build_argb8888_shm_buffer,
    build_shm_buffer_from_pixels, create_dmabuf_buffer, retire_buffer,
};

const DMABUF_POOL_CAP: usize = 16;

pub(crate) enum LayerBackend {
    Gpu(Arc<GpuContext>),
    Shm,
}

#[derive(Copy, Clone, PartialEq, Eq)]
enum Kind {
    Gpu,
    Shm,
}

struct ShmRect {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    pixels: Vec<u8>,
}

struct GpuPayload {
    pixels: Vec<u8>,
    dirty: Vec<DirtyRect>,
    width: u32,
    height: u32,
    stride: u32,
}

struct ShmPayload {
    rects: Vec<ShmRect>,
    full_pixels: Option<Vec<u8>>,
    width: i32,
    height: i32,
}

enum PendingFrame {
    Gpu(GpuPayload),
    Shm(ShmPayload),
    Dmabuf(JfnDmabufFrame),
    Placeholder(u8, u8, u8),
}

/// `InFlight` covers the window after the worker takes the queued surface but
/// before it has committed the proxy; `drain_popup` must wait on it too, or a
/// destroy races that pending `.commit()`.
enum PopupCommit {
    Idle,
    Queued(WlSurface),
    InFlight,
}

/// Every event that invalidates the shadow (dmabuf, placeholder, hide, resize)
/// must reset it to `Stale`, or a later dirty-only frame patches stale pixels.
enum ShadowState {
    Stale,
    Valid { size: (i32, i32) },
}

struct Mailbox {
    pending: Option<PendingFrame>,
    shadow: ShadowState,
    viewport: ViewportState,
    visible: bool,
    hide_pending: bool,
    viewport_dirty: bool,
    shutdown: bool,
    popup: PopupCommit,
}

impl Mailbox {
    fn new(viewport: ViewportState, visible: bool) -> Self {
        Self {
            pending: None,
            shadow: ShadowState::Stale,
            viewport,
            visible,
            hide_pending: false,
            viewport_dirty: false,
            shutdown: false,
            popup: PopupCommit::Idle,
        }
    }

    fn set_visible(&mut self, visible: bool) {
        self.visible = visible;
        if !visible {
            self.pending = None;
            self.hide_pending = true;
            self.shadow = ShadowState::Stale;
        }
    }

    fn resize(&mut self, viewport: ViewportState) {
        // Callers invoke this per frame; without the guard an unchanged extent
        // would stale the shadow every frame and defeat dirty-only coalescing.
        if self.viewport == viewport {
            return;
        }
        self.viewport = viewport;
        self.viewport_dirty = true;
        self.shadow = ShadowState::Stale;
    }

    fn request_placeholder(&mut self, r: u8, g: u8, b: u8) {
        self.pending = Some(PendingFrame::Placeholder(r, g, b));
        self.shadow = ShadowState::Stale;
    }

    fn present_dmabuf(&mut self, frame: JfnDmabufFrame) {
        self.pending = Some(PendingFrame::Dmabuf(frame));
        self.shadow = ShadowState::Stale;
    }

    fn enqueue_gpu(&mut self, payload: GpuPayload) {
        self.pending = Some(PendingFrame::Gpu(payload));
    }

    fn needs_full_copy(&self, width: i32, height: i32) -> bool {
        !matches!(self.shadow, ShadowState::Valid { size } if size == (width, height))
            || matches!(
                &self.pending,
                Some(PendingFrame::Shm(ShmPayload {
                    full_pixels: Some(_),
                    ..
                }))
            )
    }

    /// Merges dirty rects into a co-pending dirty-only frame of the same size
    /// instead of replacing it and dropping the earlier rects.
    fn store_shm(
        &mut self,
        rects: Vec<ShmRect>,
        full_pixels: Option<Vec<u8>>,
        width: i32,
        height: i32,
    ) {
        if full_pixels.is_some() {
            self.shadow = ShadowState::Valid {
                size: (width, height),
            };
        }
        if full_pixels.is_none()
            && let Some(PendingFrame::Shm(existing)) = self.pending.as_mut()
            && existing.full_pixels.is_none()
            && existing.width == width
            && existing.height == height
        {
            existing.rects.extend(rects);
            return;
        }
        self.pending = Some(PendingFrame::Shm(ShmPayload {
            rects,
            full_pixels,
            width,
            height,
        }));
    }
}

#[derive(Debug, PartialEq, Eq)]
enum Route {
    Gpu,
    Shm,
}

fn route_software(kind: Kind, gpu_failed: bool) -> Route {
    match kind {
        Kind::Gpu if !gpu_failed => Route::Gpu,
        _ => Route::Shm,
    }
}

fn validate_present_dims(width: i32, height: i32) -> Result<(), PresentError> {
    if width <= 0 || height <= 0 {
        return Err(PresentError::BadDimensions(width, height));
    }
    Ok(())
}

pub(crate) struct LayerActor {
    kind: Kind,
    shared: Arc<(Mutex<Mailbox>, Condvar)>,
    gpu_failed: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl LayerActor {
    pub(crate) fn new(
        backend: LayerBackend,
        qh: QueueHandle<DispatchState>,
        shm: WlShm,
        dmabuf: Option<ZwpLinuxDmabufV1>,
        layer: LayerSurface,
        viewport_state: ViewportState,
        visible: bool,
    ) -> Self {
        let kind = match backend {
            LayerBackend::Gpu(_) => Kind::Gpu,
            LayerBackend::Shm => Kind::Shm,
        };
        let shared = Arc::new((
            Mutex::new(Mailbox::new(viewport_state, visible)),
            Condvar::new(),
        ));
        let gpu_failed = Arc::new(AtomicBool::new(false));
        let worker_shared = Arc::clone(&shared);
        let worker_failed = Arc::clone(&gpu_failed);
        let thread = thread::spawn(move || {
            run(
                backend,
                qh,
                shm,
                dmabuf,
                layer,
                worker_shared,
                worker_failed,
            )
        });
        Self {
            kind,
            shared,
            gpu_failed,
            thread: Some(thread),
        }
    }

    fn with_state(&self, f: impl FnOnce(&mut Mailbox)) {
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
        f(&mut state);
        cv.notify_one();
    }

    pub(crate) fn resize(&self, lw: i32, lh: i32, pw: i32, ph: i32) {
        if pw <= 0 || ph <= 0 {
            return;
        }
        self.with_state(|s| s.resize(ViewportState { lw, lh, pw, ph }));
    }

    pub(crate) fn set_visible(&self, visible: bool) {
        self.with_state(|s| s.set_visible(visible));
    }

    pub(crate) fn request_placeholder(&self, r: u8, g: u8, b: u8) {
        self.with_state(|s| s.request_placeholder(r, g, b));
    }

    /// Hand a synchronized popup subsurface to the worker, which commits the
    /// popup and its parent layer back-to-back so the popup's cached buffer
    /// applies in one ordered flush rather than racing a cross-thread commit.
    pub(crate) fn commit_popup(&self, popup: WlSurface) {
        self.with_state(|s| s.popup = PopupCommit::Queued(popup));
    }

    /// Block until the worker owes no popup commit. The `shutdown` term is
    /// required: a commit still queued as the worker breaks its loop would
    /// otherwise never be consumed and hang the wait forever.
    pub(crate) fn drain_popup(&self) {
        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
        while !matches!(state.popup, PopupCommit::Idle) && !state.shutdown {
            state = cv.wait(state).unwrap_or_else(PoisonError::into_inner);
        }
    }

    pub(crate) fn present_dmabuf(&self, frame: JfnDmabufFrame) -> Result<Present, PresentError> {
        validate_present_dims(frame.coded_w, frame.coded_h)?;
        self.with_state(|s| s.present_dmabuf(frame));
        Ok(Present::Committed)
    }

    pub(crate) fn present_software(
        &self,
        pixels: &[u8],
        width: i32,
        height: i32,
        dirty: &[JfnRect],
    ) -> Result<Present, PresentError> {
        validate_present_dims(width, height)?;
        let stride = (width as usize).saturating_mul(4);
        let Some(len) = (height as usize).checked_mul(stride) else {
            return Err(PresentError::BadDimensions(width, height));
        };
        if pixels.len() < len {
            return Err(PresentError::ShortBuffer {
                have: pixels.len(),
                need: len,
            });
        }
        match route_software(self.kind, self.gpu_failed.load(Ordering::Acquire)) {
            Route::Gpu => self.enqueue_gpu(pixels, len, width, height, stride, dirty),
            Route::Shm => self.enqueue_shm(pixels, len, width, height, stride, dirty),
        }
    }

    fn enqueue_gpu(
        &self,
        pixels: &[u8],
        len: usize,
        width: i32,
        height: i32,
        stride: usize,
        dirty: &[JfnRect],
    ) -> Result<Present, PresentError> {
        let dirty = dirty
            .iter()
            .map(|r| DirtyRect {
                x: r.x,
                y: r.y,
                w: r.w,
                h: r.h,
            })
            .collect();
        self.with_state(|s| {
            s.enqueue_gpu(GpuPayload {
                pixels: pixels[..len].to_vec(),
                dirty,
                width: width as u32,
                height: height as u32,
                stride: stride as u32,
            });
        });
        Ok(Present::Committed)
    }

    fn enqueue_shm(
        &self,
        pixels: &[u8],
        len: usize,
        width: i32,
        height: i32,
        stride: usize,
        dirty: &[JfnRect],
    ) -> Result<Present, PresentError> {
        let rects: Vec<ShmRect> = dirty
            .iter()
            .filter_map(|rect| copy_dirty_rect(pixels, stride, width, height, rect))
            .collect();

        let (lock, cv) = &*self.shared;
        let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
        let full_pixels = state
            .needs_full_copy(width, height)
            .then(|| pixels[..len].to_vec());
        if rects.is_empty() && full_pixels.is_none() {
            return Ok(Present::Skipped);
        }
        state.store_shm(rects, full_pixels, width, height);
        drop(state);
        cv.notify_one();
        Ok(Present::Committed)
    }

    pub(crate) fn shutdown(mut self) {
        self.with_state(|s| {
            s.shutdown = true;
            s.pending = None;
        });
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

// ===================================================================
// Worker loop decision (pure)
// ===================================================================

/// The primary frame op for one worker iteration.
#[derive(Debug, PartialEq)]
pub(crate) enum Action<F> {
    Hide,
    Present(F),
    BareCommit,
    ReapplyViewport,
    Nop,
}

pub(crate) struct Decision<F> {
    pub commit_popup: bool,
    pub action: Action<F>,
}

fn next_content<F>(prev: bool, action: &Action<F>, committed: bool, is_placeholder: bool) -> bool {
    match action {
        Action::Hide => false,
        Action::Present(_) if committed && !is_placeholder => true,
        _ => prev,
    }
}

/// Decide the popup commit and frame op from a mailbox snapshot. Driven by the
/// final desired `visible` state: a frame arriving in the same wake as a
/// coalesced hide+show is presented, not dropped.
///
/// # Examples
/// ```ignore
/// let d = decide(Some(7u32), false, true, false, false, false);
/// assert_eq!(d.action, Action::Present(7));
/// ```
pub(crate) fn decide<F>(
    pending: Option<F>,
    pending_is_placeholder: bool,
    visible: bool,
    has_content: bool,
    viewport_dirty: bool,
    popup_pending: bool,
) -> Decision<F> {
    let action = if !visible {
        Action::Hide
    } else if let Some(frame) = pending {
        if pending_is_placeholder && has_content {
            Action::Nop
        } else {
            Action::Present(frame)
        }
    } else {
        match reconcile(viewport_dirty, popup_pending) {
            Reconcile::ReapplyViewport => Action::ReapplyViewport,
            Reconcile::BareCommit => Action::BareCommit,
            Reconcile::None => Action::Nop,
        }
    };
    Decision {
        commit_popup: popup_pending,
        action,
    }
}

/// The fallback commit a still-visible layer owes when its frame op did not
/// commit. Viewport is checked first because reapplying it also commits, which
/// folds any co-pending popup — returning `BareCommit` there would drop it.
#[derive(Debug, PartialEq, Eq)]
enum Reconcile {
    ReapplyViewport,
    BareCommit,
    None,
}

fn reconcile(viewport_dirty: bool, popup_pending: bool) -> Reconcile {
    if viewport_dirty {
        Reconcile::ReapplyViewport
    } else if popup_pending {
        Reconcile::BareCommit
    } else {
        Reconcile::None
    }
}

// ===================================================================
// Actor thread
// ===================================================================

#[derive(Default)]
struct ShmShadow {
    pixels: Vec<u8>,
    size: (i32, i32),
}

enum Backend {
    Gpu { painter: Option<Box<GpuPainter>> },
    Shm { shadow: ShmShadow },
}

fn hide_detaches(backend: &Backend) -> bool {
    matches!(backend, Backend::Shm { .. })
}

/// Only a GPU failure degrades: dmabuf has no CPU fallback, so latching it to
/// shm would strand the surface with no output.
fn is_degrading_error(err: &PresentError) -> bool {
    matches!(err, PresentError::Gpu(_))
}

fn degrade(backend: &mut Backend, gpu_failed: &AtomicBool) -> Option<Box<GpuPainter>> {
    let old = match backend {
        Backend::Gpu { painter } => painter.take(),
        Backend::Shm { .. } => None,
    };
    *backend = Backend::Shm {
        shadow: ShmShadow::default(),
    };
    gpu_failed.store(true, Ordering::Release);
    old
}

struct Runner {
    qh: QueueHandle<DispatchState>,
    shm: WlShm,
    dmabuf: Option<ZwpLinuxDmabufV1>,
    backend: Backend,
    gpu_ctx: Option<Arc<GpuContext>>,
    gpu_failed: Arc<AtomicBool>,
    /// Gates present-failure logging to the first failure of a failing streak.
    present_failing: bool,
    dmabuf_pool: Vec<DmabufBuf>,
    /// Held until the compositor releases it: an attached buffer must outlive
    /// its use by the compositor.
    current: Option<OwnedBuffer>,
}

fn run(
    backend: LayerBackend,
    qh: QueueHandle<DispatchState>,
    shm: WlShm,
    dmabuf: Option<ZwpLinuxDmabufV1>,
    layer: LayerSurface,
    shared: Arc<(Mutex<Mailbox>, Condvar)>,
    gpu_failed: Arc<AtomicBool>,
) {
    let (backend, gpu_ctx) = match backend {
        LayerBackend::Gpu(ctx) => (Backend::Gpu { painter: None }, Some(ctx)),
        LayerBackend::Shm => (
            Backend::Shm {
                shadow: ShmShadow::default(),
            },
            None,
        ),
    };
    let mut runner = Runner {
        qh,
        shm,
        dmabuf,
        backend,
        gpu_ctx,
        gpu_failed,
        present_failing: false,
        dmabuf_pool: Vec::new(),
        current: None,
    };
    let mut has_content = false;

    loop {
        let (
            pending,
            pending_is_placeholder,
            popup_commit,
            viewport,
            visible,
            viewport_dirty,
            shutdown,
        ) = {
            let (lock, cv) = &*shared;
            let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
            while state.pending.is_none()
                && !state.shutdown
                && !state.hide_pending
                && !state.viewport_dirty
                && !matches!(state.popup, PopupCommit::Queued(_))
            {
                state = cv.wait(state).unwrap_or_else(PoisonError::into_inner);
            }
            state.hide_pending = false;
            let viewport_dirty = state.viewport_dirty;
            state.viewport_dirty = false;
            let popup_commit = if let PopupCommit::Queued(popup) =
                std::mem::replace(&mut state.popup, PopupCommit::InFlight)
            {
                Some(popup)
            } else {
                state.popup = PopupCommit::Idle;
                None
            };
            let pending = state.pending.take();
            let pending_is_placeholder = matches!(pending, Some(PendingFrame::Placeholder(..)));
            (
                pending,
                pending_is_placeholder,
                popup_commit,
                state.viewport,
                state.visible,
                viewport_dirty,
                state.shutdown,
            )
        };

        if shutdown {
            break;
        }

        let decision = decide(
            pending,
            pending_is_placeholder,
            visible,
            has_content,
            viewport_dirty,
            popup_commit.is_some(),
        );

        // Commit the popup before the layer commit that follows (the frame op or
        // the reconcile below), which folds the popup's cached state.
        if decision.commit_popup
            && let Some(popup) = popup_commit.as_ref()
        {
            popup.commit();
            let (lock, cv) = &*shared;
            let mut state = lock.lock().unwrap_or_else(PoisonError::into_inner);
            // A `Queued(next)` that arrived mid-commit is a fresh obligation and
            // must survive; only clear our own in-flight commit.
            if matches!(state.popup, PopupCommit::InFlight) {
                state.popup = PopupCommit::Idle;
            }
            drop(state);
            cv.notify_one();
        }

        let action = decision.action;
        let mut present_committed = false;
        let mut layer_committed = match &action {
            Action::Hide => runner.hide(&layer),
            Action::Present(frame) => match runner.present(frame, &layer, viewport) {
                Ok(Present::Committed) => {
                    runner.present_failing = false;
                    present_committed = true;
                    true
                }
                Ok(Present::Skipped) => false,
                Err(err) => {
                    runner.on_present_error(err);
                    false
                }
            },
            Action::ReapplyViewport => {
                // Zero source args leave the latched source untouched; only the
                // destination is rescaled to the new logical size.
                layer.set_viewport(0, 0, viewport.lw, viewport.lh);
                layer.commit();
                true
            }
            Action::BareCommit => {
                layer.commit();
                true
            }
            Action::Nop => false,
        };
        has_content = next_content(
            has_content,
            &action,
            present_committed,
            pending_is_placeholder,
        );

        // The `visible` gate keeps this fallback commit off a hidden GPU/WSI
        // surface, whose buffers the compositor's swapchain owns.
        if visible && !layer_committed {
            match reconcile(viewport_dirty, popup_commit.is_some()) {
                Reconcile::ReapplyViewport => {
                    // Zero source args leave the latched source untouched; only
                    // the destination is rescaled to the new logical size.
                    layer.set_viewport(0, 0, viewport.lw, viewport.lh);
                    layer.commit();
                    layer_committed = true;
                }
                Reconcile::BareCommit => {
                    layer.commit();
                    layer_committed = true;
                }
                Reconcile::None => {}
            }
        }

        if layer_committed || popup_commit.is_some() {
            layer.flush();
            crate::root_window::request_present();
        }
    }

    runner.shutdown();
}

impl Runner {
    fn set_current(&mut self, buf: Option<OwnedBuffer>) {
        if let Some(old) = self.current.take() {
            retire_buffer(old);
        }
        self.current = buf;
    }

    /// Returns whether the layer surface was committed. The GPU path leaves the
    /// surface untouched — Vulkan WSI owns its buffers and an external
    /// null-attach + commit would fight the swapchain — so it returns `false`.
    fn hide(&mut self, layer: &LayerSurface) -> bool {
        if let Backend::Gpu { painter } = &mut self.backend
            && let Some(painter) = painter.as_mut()
        {
            painter.set_visible(false);
        }
        if hide_detaches(&self.backend) {
            layer.attach_none();
            layer.commit();
            self.set_current(None);
            true
        } else {
            false
        }
    }

    fn on_present_error(&mut self, err: PresentError) {
        let degraded = is_degrading_error(&err);
        if degraded {
            let old = degrade(&mut self.backend, &self.gpu_failed);
            if let Some(painter) = old {
                painter.shutdown();
            }
        }
        if !self.present_failing {
            self.present_failing = true;
            tracing::warn!(error = %err, degraded, "wayland layer actor: present failed");
        }
    }

    fn present(
        &mut self,
        frame: &PendingFrame,
        layer: &LayerSurface,
        vps: ViewportState,
    ) -> Result<Present, PresentError> {
        match frame {
            PendingFrame::Gpu(p) => self.present_gpu(layer, vps, p),
            PendingFrame::Shm(p) => self.present_shm(layer, vps, p),
            PendingFrame::Dmabuf(frame) => self.present_dmabuf(layer, vps, frame),
            PendingFrame::Placeholder(r, g, b) => {
                self.present_placeholder(layer, vps, (*r, *g, *b))
            }
        }
    }

    fn present_placeholder(
        &mut self,
        layer: &LayerSurface,
        vps: ViewportState,
        bg: (u8, u8, u8),
    ) -> Result<Present, PresentError> {
        let (r, g, b) = bg;
        let Some(buf) =
            build_argb8888_shm_buffer(&self.shm, &self.qh, "layer-placeholder", 1, 1, |dst| {
                // ARGB8888 little-endian byte order = [B, G, R, A].
                dst.copy_from_slice(&[b, g, r, 0xFF]);
                true
            })
        else {
            return Err(PresentError::ShmAlloc);
        };
        layer.present(FrameCommit::new(&buf, 1, 1, 1, 1, vps.lw, vps.lh));
        self.set_current(Some(buf));
        Ok(Present::Committed)
    }

    fn present_gpu(
        &mut self,
        layer: &LayerSurface,
        vps: ViewportState,
        p: &GpuPayload,
    ) -> Result<Present, PresentError> {
        let (Backend::Gpu { painter }, Some(ctx)) = (&mut self.backend, &self.gpu_ctx) else {
            return Ok(Present::Skipped);
        };
        if painter.is_none() {
            let Some(target) = layer.window_target() else {
                return Ok(Present::Skipped);
            };
            let new = GpuPainter::new(ctx.clone(), target, (p.width, p.height))?;
            *painter = Some(Box::new(new));
        }
        let Some(painter) = painter.as_mut() else {
            return Ok(Present::Skipped);
        };
        painter.set_visible(true);
        painter.resize((vps.pw.max(1) as u32, vps.ph.max(1) as u32));
        let pixel_frame = PixelFrame {
            width: p.width,
            height: p.height,
            stride: p.stride,
            bgra: &p.pixels,
            dirty: &p.dirty,
        };
        // Set the viewport source inside the present closure, not here: a
        // dropped frame must not leave a source pending ahead of the next
        // buffer. Clamped to min(buffer, physical) to stay within bounds.
        let src_w = (p.width as i32).min(vps.pw);
        let src_h = (p.height as i32).min(vps.ph);
        painter.push_pixels(pixel_frame, || {
            layer.set_viewport(src_w, src_h, vps.lw, vps.lh)
        })?;
        Ok(Present::Committed)
    }

    fn present_shm(
        &mut self,
        layer: &LayerSurface,
        vps: ViewportState,
        p: &ShmPayload,
    ) -> Result<Present, PresentError> {
        let (width, height) = (p.width, p.height);
        let Backend::Shm { shadow } = &mut self.backend else {
            return Ok(Present::Skipped);
        };
        compose_shm_shadow(shadow, p)?;
        let Some(buf) = build_shm_buffer_from_pixels(
            &self.shm,
            &self.qh,
            "cef-sw-worker",
            &shadow.pixels,
            width,
            height,
        ) else {
            return Err(PresentError::ShmAlloc);
        };
        layer.present(FrameCommit::new(
            &buf,
            width,
            height,
            width.min(vps.pw),
            height.min(vps.ph),
            vps.lw,
            vps.lh,
        ));
        self.set_current(Some(buf));
        Ok(Present::Committed)
    }

    fn present_dmabuf(
        &mut self,
        layer: &LayerSurface,
        vps: ViewportState,
        frame: &JfnDmabufFrame,
    ) -> Result<Present, PresentError> {
        let vw = if frame.visible_w > 0 {
            frame.visible_w
        } else {
            frame.coded_w
        };
        let vh = if frame.visible_h > 0 {
            frame.visible_h
        } else {
            frame.coded_h
        };
        let Some(pos) = self.lease_dmabuf(frame) else {
            return Err(PresentError::DmabufCreate);
        };
        let (cw, ch) = (frame.coded_w, frame.coded_h);
        match pos {
            DmabufLease::Pooled => {
                layer.present(FrameCommit::new(
                    &self.dmabuf_pool[0].buf,
                    cw,
                    ch,
                    vw,
                    vh,
                    vps.lw,
                    vps.lh,
                ));
                self.set_current(None);
            }
            DmabufLease::OneShot(buf) => {
                layer.present(FrameCommit::new(&buf, cw, ch, vw, vh, vps.lw, vps.lh));
                self.set_current(Some(buf));
            }
        }
        Ok(Present::Committed)
    }

    fn lease_dmabuf(&mut self, frame: &JfnDmabufFrame) -> Option<DmabufLease> {
        let dmabuf = self.dmabuf.as_ref()?;
        let Some(id) = frame.id else {
            let buf = create_dmabuf_buffer(
                dmabuf,
                &self.qh,
                frame.fd.as_fd(),
                frame.stride,
                frame.modifier,
                frame.coded_w,
                frame.coded_h,
            )?;
            return Some(DmabufLease::OneShot(buf));
        };

        let hit = self.dmabuf_pool.iter().position(|e| {
            e.id == id
                && e.w == frame.coded_w
                && e.h == frame.coded_h
                && e.stride == frame.stride
                && e.modifier == frame.modifier
        });
        if let Some(pos) = hit {
            if buffer_is_idle(&self.dmabuf_pool[pos].buf) {
                let entry = self.dmabuf_pool.remove(pos);
                self.dmabuf_pool.insert(0, entry);
                return Some(DmabufLease::Pooled);
            }
            retire_buffer(self.dmabuf_pool.remove(pos).buf);
        }
        if let Some(stale) = self.dmabuf_pool.iter().position(|e| e.id == id) {
            retire_buffer(self.dmabuf_pool.remove(stale).buf);
        }

        let buf = create_dmabuf_buffer(
            dmabuf,
            &self.qh,
            frame.fd.as_fd(),
            frame.stride,
            frame.modifier,
            frame.coded_w,
            frame.coded_h,
        )?;
        self.dmabuf_pool.insert(
            0,
            DmabufBuf {
                id,
                w: frame.coded_w,
                h: frame.coded_h,
                stride: frame.stride,
                modifier: frame.modifier,
                buf,
            },
        );
        while self.dmabuf_pool.len() > DMABUF_POOL_CAP {
            if let Some(evicted) = self.dmabuf_pool.pop() {
                retire_buffer(evicted.buf);
            }
        }
        Some(DmabufLease::Pooled)
    }

    fn shutdown(mut self) {
        self.set_current(None);
        for entry in self.dmabuf_pool.drain(..) {
            retire_buffer(entry.buf);
        }
        if let Backend::Gpu {
            painter: Some(painter),
        } = self.backend
        {
            painter.shutdown();
        }
    }
}

enum DmabufLease {
    Pooled,
    OneShot(OwnedBuffer),
}

fn compose_shm_shadow(shadow: &mut ShmShadow, payload: &ShmPayload) -> Result<(), PresentError> {
    let (width, height) = (payload.width, payload.height);
    if shadow.size != (width, height) {
        let stride = (width as usize).saturating_mul(4);
        let Some(size) = (height as usize).checked_mul(stride) else {
            return Err(PresentError::BadDimensions(width, height));
        };
        shadow.pixels.clear();
        shadow.pixels.resize(size, 0);
        shadow.size = (width, height);
    }
    if let Some(full_pixels) = payload.full_pixels.as_deref()
        && let Some(dst) = shadow.pixels.get_mut(..full_pixels.len())
    {
        dst.copy_from_slice(full_pixels);
    }
    apply_dirty_to_shadow(&mut shadow.pixels, width, &payload.rects);
    Ok(())
}

fn copy_dirty_rect(
    pixels: &[u8],
    src_stride: usize,
    width: i32,
    height: i32,
    rect: &JfnRect,
) -> Option<ShmRect> {
    // Clamp in i64 so a CEF-supplied `x + w` / `y + h` cannot overflow i32.
    let x0 = i64::from(rect.x).max(0);
    let y0 = i64::from(rect.y).max(0);
    let x1 = (i64::from(rect.x) + i64::from(rect.w)).min(i64::from(width));
    let y1 = (i64::from(rect.y) + i64::from(rect.h)).min(i64::from(height));
    if x1 <= x0 || y1 <= y0 {
        return None;
    }
    let rx = x0 as i32;
    let ry = y0 as i32;
    let rw = (x1 - x0) as i32;
    let rh = (y1 - y0) as i32;

    let (Ok(rw_us), Ok(rx_us)) = (usize::try_from(rw), usize::try_from(rx)) else {
        return None;
    };
    let (Some(row_bytes), Some(rx_bytes)) = (rw_us.checked_mul(4), rx_us.checked_mul(4)) else {
        return None;
    };
    let mut out = Vec::with_capacity(row_bytes.saturating_mul(usize::try_from(rh).unwrap_or(0)));
    for row in ry..(ry + rh) {
        let Ok(row_us) = usize::try_from(row) else {
            continue;
        };
        let Some(off) = row_us
            .checked_mul(src_stride)
            .and_then(|v| v.checked_add(rx_bytes))
        else {
            continue;
        };
        let Some(end) = off.checked_add(row_bytes) else {
            continue;
        };
        let Some(slice) = pixels.get(off..end) else {
            continue;
        };
        out.extend_from_slice(slice);
    }

    Some(ShmRect {
        x: rx,
        y: ry,
        w: rw,
        h: rh,
        pixels: out,
    })
}

fn apply_dirty_to_shadow(shadow: &mut [u8], width: i32, rects: &[ShmRect]) {
    let Ok(width_us) = usize::try_from(width) else {
        return;
    };
    let Some(dst_stride) = width_us.checked_mul(4) else {
        return;
    };
    for rect in rects {
        let (Ok(rw_us), Ok(rx_us)) = (usize::try_from(rect.w), usize::try_from(rect.x)) else {
            continue;
        };
        let (Some(row_bytes), Some(rx_bytes)) = (rw_us.checked_mul(4), rx_us.checked_mul(4)) else {
            continue;
        };
        for row in 0..rect.h {
            let Ok(row_us) = usize::try_from(row) else {
                continue;
            };
            let Some(dst_row) = rect
                .y
                .checked_add(row)
                .and_then(|y| usize::try_from(y).ok())
            else {
                continue;
            };
            let Some(src_off) = row_us.checked_mul(row_bytes) else {
                continue;
            };
            let Some(dst_off) = dst_row
                .checked_mul(dst_stride)
                .and_then(|v| v.checked_add(rx_bytes))
            else {
                continue;
            };
            let (Some(src_end), Some(dst_end)) = (
                src_off.checked_add(row_bytes),
                dst_off.checked_add(row_bytes),
            ) else {
                continue;
            };
            let (Some(src), Some(dst)) = (
                rect.pixels.get(src_off..src_end),
                shadow.get_mut(dst_off..dst_end),
            ) else {
                continue;
            };
            dst.copy_from_slice(src);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vp() -> ViewportState {
        ViewportState {
            lw: 100,
            lh: 100,
            pw: 100,
            ph: 100,
        }
    }

    fn rect(x: i32, y: i32, w: i32, h: i32) -> JfnRect {
        JfnRect { x, y, w, h }
    }

    #[test]
    fn coalesced_hide_then_show_frame_presents() {
        let d = decide(Some(7u32), false, true, false, false, false);
        assert_eq!(d.action, Action::Present(7));
    }

    #[test]
    fn hide_alone_hides() {
        let d = decide(None::<u32>, false, false, true, false, false);
        assert_eq!(d.action, Action::Hide);
    }

    #[test]
    fn placeholder_honored_again_after_hide() {
        let d = decide(Some(0u32), true, true, false, false, false);
        assert_eq!(d.action, Action::Present(0));
    }

    #[test]
    fn placeholder_skipped_when_content_present() {
        let d = decide(Some(0u32), true, true, true, false, false);
        assert_eq!(d.action, Action::Nop);
    }

    #[test]
    fn next_content_transition_table() {
        // Hide clears regardless of prior state or commit.
        assert!(!next_content(true, &Action::<()>::Hide, true, false));
        // A committed non-placeholder present sets it.
        assert!(next_content(false, &Action::Present(()), true, false));
        // A placeholder present never sets it.
        assert!(!next_content(false, &Action::Present(()), true, true));
        // A skipped/failed present (not committed) leaves the prior value.
        assert!(next_content(true, &Action::Present(()), false, false));
        assert!(!next_content(false, &Action::Present(()), false, false));
        // Bare/viewport/nop leave the prior value.
        assert!(next_content(true, &Action::<()>::BareCommit, true, false));
        assert!(!next_content(
            false,
            &Action::<()>::ReapplyViewport,
            true,
            false
        ));
        assert!(next_content(true, &Action::<()>::Nop, false, false));
    }

    #[test]
    fn present_and_popup_decided_from_one_snapshot() {
        let d = decide(Some(7u32), false, true, false, false, true);
        assert!(d.commit_popup);
        assert_eq!(d.action, Action::Present(7));
    }

    #[test]
    fn popup_only_snapshot_bare_commits() {
        let d = decide(None::<u32>, false, true, false, false, true);
        assert!(d.commit_popup);
        assert_eq!(d.action, Action::BareCommit);
    }

    #[test]
    fn viewport_dirty_snapshot_reapplies_viewport() {
        let d = decide(None::<u32>, false, true, false, true, true);
        assert_eq!(d.action, Action::ReapplyViewport);
    }

    #[test]
    fn popup_survives_a_skipped_or_failed_present() {
        assert_eq!(
            decide(Some(9u32), false, true, false, false, true).action,
            Action::Present(9)
        );
        assert_eq!(reconcile(false, true), Reconcile::BareCommit);
    }

    #[test]
    fn reconcile_bare_commits_for_popup() {
        assert_eq!(reconcile(false, true), Reconcile::BareCommit);
        assert_eq!(reconcile(false, false), Reconcile::None);
    }

    #[test]
    fn reconcile_reapplies_viewport() {
        assert_eq!(reconcile(true, false), Reconcile::ReapplyViewport);
        assert_eq!(reconcile(true, true), Reconcile::ReapplyViewport);
    }

    #[test]
    fn store_shm_merges_dirty_only_same_dims() {
        let mut mb = Mailbox::new(vp(), true);
        mb.store_shm(vec![test_rect(1)], None, 100, 100);
        mb.store_shm(vec![test_rect(2)], None, 100, 100);
        let Some(PendingFrame::Shm(p)) = &mb.pending else {
            panic!("expected shm payload");
        };
        assert_eq!(p.rects.len(), 2);
    }

    #[test]
    fn store_shm_replaces_on_dim_mismatch() {
        let mut mb = Mailbox::new(vp(), true);
        mb.store_shm(vec![test_rect(1)], None, 100, 100);
        mb.store_shm(vec![test_rect(2)], None, 200, 200);
        let Some(PendingFrame::Shm(p)) = &mb.pending else {
            panic!("expected shm payload");
        };
        assert_eq!(p.rects.len(), 1);
        assert_eq!((p.width, p.height), (200, 200));
    }

    #[test]
    fn store_shm_replaces_when_pending_has_full() {
        let mut mb = Mailbox::new(vp(), true);
        mb.store_shm(vec![], Some(vec![0u8; 4]), 1, 1);
        mb.store_shm(vec![test_rect(2)], None, 1, 1);
        let Some(PendingFrame::Shm(p)) = &mb.pending else {
            panic!("expected shm payload");
        };
        assert!(p.full_pixels.is_none());
        assert_eq!(p.rects.len(), 1);
    }

    fn test_rect(tag: u8) -> ShmRect {
        ShmRect {
            x: 0,
            y: 0,
            w: 1,
            h: 1,
            pixels: vec![tag; 4],
        }
    }

    #[test]
    fn full_copy_marks_shadow_valid() {
        let mut mb = Mailbox::new(vp(), true);
        assert!(matches!(mb.shadow, ShadowState::Stale));
        mb.store_shm(vec![], Some(vec![0u8; 4]), 1, 1);
        assert!(matches!(mb.shadow, ShadowState::Valid { size: (1, 1) }));
    }

    #[test]
    fn valid_shadow_at_wrong_size_still_full_copies() {
        let mut mb = Mailbox::new(vp(), true);
        mb.store_shm(vec![], Some(vec![0u8; 4 * 100 * 100]), 100, 100);
        mb.pending = None; // worker consumed the full frame
        assert!(!mb.needs_full_copy(100, 100));
        assert!(mb.needs_full_copy(200, 200));
    }

    #[test]
    fn placeholder_invalidates_shadow_forcing_full_copy() {
        let mut mb = Mailbox::new(vp(), true);
        mb.store_shm(vec![], Some(vec![0u8; 4 * 100 * 100]), 100, 100);
        assert!(matches!(mb.shadow, ShadowState::Valid { .. }));
        mb.pending = None; // worker consumed the full frame
        assert!(!mb.needs_full_copy(100, 100));
        mb.request_placeholder(0, 0, 0);
        assert!(matches!(mb.shadow, ShadowState::Stale));
        assert!(mb.needs_full_copy(100, 100));
    }

    fn dmabuf_frame(coded_w: i32, coded_h: i32) -> JfnDmabufFrame {
        let fd = std::fs::File::open("/dev/null").unwrap().into();
        JfnDmabufFrame {
            fd,
            id: None,
            stride: 0,
            modifier: 0,
            coded_w,
            coded_h,
            visible_w: coded_w,
            visible_h: coded_h,
        }
    }

    fn valid_shadow() -> ShadowState {
        ShadowState::Valid { size: (100, 100) }
    }

    #[test]
    fn resize_noop_when_unchanged() {
        let mut mb = Mailbox::new(vp(), true);
        mb.shadow = valid_shadow();
        mb.resize(vp());
        assert!(!mb.viewport_dirty);
        assert!(matches!(mb.shadow, ShadowState::Valid { .. }));

        mb.resize(ViewportState {
            lw: 200,
            lh: 200,
            pw: 200,
            ph: 200,
        });
        assert!(mb.viewport_dirty);
        assert!(matches!(mb.shadow, ShadowState::Stale));
    }

    #[test]
    fn dmabuf_hide_and_resize_invalidate_shadow() {
        let mut mb = Mailbox::new(vp(), true);
        mb.shadow = valid_shadow();
        mb.present_dmabuf(dmabuf_frame(64, 64));
        assert!(matches!(mb.shadow, ShadowState::Stale));

        mb.shadow = valid_shadow();
        mb.set_visible(false);
        assert!(matches!(mb.shadow, ShadowState::Stale));

        let mut mb = Mailbox::new(vp(), true);
        mb.shadow = valid_shadow();
        mb.resize(ViewportState {
            lw: 200,
            lh: 200,
            pw: 200,
            ph: 200,
        });
        assert!(matches!(mb.shadow, ShadowState::Stale));
    }

    #[test]
    fn route_software_falls_back_after_gpu_failure() {
        assert_eq!(route_software(Kind::Gpu, false), Route::Gpu);
        assert_eq!(route_software(Kind::Gpu, true), Route::Shm);
        assert_eq!(route_software(Kind::Shm, false), Route::Shm);
        assert_eq!(route_software(Kind::Shm, true), Route::Shm);
    }

    #[test]
    fn gpu_hide_performs_no_surface_op() {
        assert!(!hide_detaches(&Backend::Gpu { painter: None }));
        assert!(hide_detaches(&Backend::Shm {
            shadow: ShmShadow::default(),
        }));
    }

    #[test]
    fn gpu_error_degrades_backend() {
        assert!(is_degrading_error(&PresentError::Gpu(
            jfn_gpu_paint::GpuPaintError::SurfaceUnsupported
        )));
        assert!(!is_degrading_error(&PresentError::ShmAlloc));
        assert!(!is_degrading_error(&PresentError::DmabufCreate));

        let mut backend = Backend::Gpu { painter: None };
        let flag = AtomicBool::new(false);
        let old = degrade(&mut backend, &flag);
        assert!(old.is_none());
        assert!(matches!(backend, Backend::Shm { .. }));
        assert!(flag.load(Ordering::Acquire));
    }

    #[test]
    fn dmabuf_producer_rejects_bad_dimensions() {
        assert!(matches!(
            validate_present_dims(0, 64),
            Err(PresentError::BadDimensions(0, 64))
        ));
        assert!(matches!(
            validate_present_dims(64, -1),
            Err(PresentError::BadDimensions(64, -1))
        ));
        assert!(validate_present_dims(64, 64).is_ok());
    }

    #[test]
    fn first_post_fallback_frame_full_copies() {
        let mb = Mailbox::new(vp(), true);
        assert!(mb.needs_full_copy(100, 100));
    }

    #[test]
    fn copy_dirty_rect_clamps_negative_origin() {
        let pixels = vec![0xABu8; 4 * 4 * 4];
        let r = copy_dirty_rect(&pixels, 16, 4, 4, &rect(-2, -2, 4, 4)).unwrap();
        assert_eq!((r.x, r.y, r.w, r.h), (0, 0, 2, 2));
        assert_eq!(r.pixels.len(), 2 * 2 * 4);
    }

    #[test]
    fn copy_dirty_rect_clamps_overflow() {
        let pixels = vec![0u8; 4 * 4 * 4];
        let r = copy_dirty_rect(&pixels, 16, 4, 4, &rect(2, 2, 10, 10)).unwrap();
        assert_eq!((r.w, r.h), (2, 2));
    }

    #[test]
    fn copy_dirty_rect_rejects_zero_area() {
        let pixels = vec![0u8; 4 * 4 * 4];
        assert!(copy_dirty_rect(&pixels, 16, 4, 4, &rect(0, 0, 0, 5)).is_none());
        assert!(copy_dirty_rect(&pixels, 16, 4, 4, &rect(4, 0, 4, 4)).is_none());
    }

    #[test]
    fn copy_dirty_rect_skips_row_past_buffer() {
        // A stride that lies about the buffer length pushes later rows out of
        // range; `get` skips them instead of panicking.
        let pixels = vec![0u8; 8];
        let r = copy_dirty_rect(&pixels, 1_000, 2, 2, &rect(0, 0, 2, 2)).unwrap();
        // First row fits (off 0..8); the second (off 1000..) is out of range.
        assert_eq!(r.pixels.len(), 8);
    }

    #[test]
    fn copy_dirty_rect_extreme_extent_does_not_panic() {
        let pixels = vec![0u8; 4 * 4 * 4];
        let r = copy_dirty_rect(&pixels, 16, 4, 4, &rect(3, 3, i32::MAX, i32::MAX)).unwrap();
        assert_eq!((r.w, r.h), (1, 1));
        assert!(copy_dirty_rect(&pixels, 16, 4, 4, &rect(i32::MAX, 0, i32::MAX, 4)).is_none());
    }

    #[test]
    fn apply_dirty_to_shadow_extreme_offsets_do_not_panic() {
        let mut shadow = vec![0u8; 4 * 2 * 2];
        let rects = vec![ShmRect {
            x: i32::MAX,
            y: i32::MAX,
            w: i32::MAX,
            h: 1,
            pixels: vec![0xFF; 4],
        }];
        apply_dirty_to_shadow(&mut shadow, 2, &rects);
        assert_eq!(shadow, vec![0u8; 16]);
    }

    #[test]
    fn apply_dirty_to_shadow_writes_offsets() {
        let mut shadow = vec![0u8; 4 * 2 * 2];
        let rects = vec![ShmRect {
            x: 1,
            y: 1,
            w: 1,
            h: 1,
            pixels: vec![0xFF; 4],
        }];
        apply_dirty_to_shadow(&mut shadow, 2, &rects);
        assert_eq!(&shadow[12..16], &[0xFF; 4]);
        assert_eq!(&shadow[0..12], &[0u8; 12]);
    }

    #[test]
    fn apply_dirty_to_shadow_skips_rect_exceeding_shadow() {
        let mut shadow = vec![0u8; 4 * 2 * 2];
        let rects = vec![ShmRect {
            x: 5,
            y: 5,
            w: 1,
            h: 1,
            pixels: vec![0xFF; 4],
        }];
        apply_dirty_to_shadow(&mut shadow, 2, &rects);
        assert_eq!(shadow, vec![0u8; 16]);
    }

    #[test]
    fn compose_shm_shadow_full_copy_is_byte_exact() {
        assert_eq!(route_software(Kind::Gpu, true), Route::Shm);

        let (w, h) = (3, 2);
        let source: Vec<u8> = (0..u8::try_from(w * h * 4).unwrap()).collect();
        let payload = ShmPayload {
            rects: Vec::new(),
            full_pixels: Some(source.clone()),
            width: w,
            height: h,
        };
        let mut shadow = ShmShadow::default();
        compose_shm_shadow(&mut shadow, &payload).unwrap();
        assert_eq!(shadow.pixels, source);
        assert_eq!(shadow.size, (w, h));
    }
}
