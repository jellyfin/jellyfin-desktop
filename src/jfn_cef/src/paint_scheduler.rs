use std::sync::Arc;
use std::sync::atomic::Ordering;

use cef::{ThreadId, post_delayed_task};

use crate::client::{Inner, TickTask, now_ns};

const BOOST_MULTIPLIER: i32 = 2;
const INVALIDATE_TICK_LIMIT: i32 = 1000;

#[derive(Debug)]
pub(crate) struct PaintMode {
    shared_textures: bool,
}

impl PaintMode {
    pub(crate) fn new(shared_textures: bool) -> Self {
        Self { shared_textures }
    }

    pub(crate) fn shared_textures(&self) -> bool {
        self.shared_textures
    }

    pub(crate) fn make_scheduler(&self) -> Box<dyn PaintScheduler> {
        make(self.shared_textures)
    }
}

pub(crate) trait PaintScheduler: Send + Sync {
    fn after_resize(&self, inner: &Arc<Inner>);
    fn kick_apply(&self, inner: &Arc<Inner>);
    fn invalidate_tick(&self, inner: &Arc<Inner>);
    fn should_present_paint(&self, inner: &Inner) -> bool;
}

struct PassivePaintScheduler;

impl PaintScheduler for PassivePaintScheduler {
    fn after_resize(&self, _inner: &Arc<Inner>) {}
    fn kick_apply(&self, _inner: &Arc<Inner>) {}
    fn invalidate_tick(&self, _inner: &Arc<Inner>) {}
    fn should_present_paint(&self, _inner: &Inner) -> bool {
        true
    }
}

struct ActivePaintScheduler;

impl PaintScheduler for ActivePaintScheduler {
    fn after_resize(&self, inner: &Arc<Inner>) {
        inner.invalidate_view();
        inner.kick_invalidate_loop();
    }

    fn kick_apply(&self, inner: &Arc<Inner>) {
        // Boost CEF compositor rate while the loop is live — JS rAF ties to
        // compositor rate, so this speeds up convergence to post-resize dims.
        let fps = inner.frame_rate.load(Ordering::Acquire);
        if inner.browser_alive() && fps > 0 && inner.saved_frame_rate.load(Ordering::Acquire) == 0 {
            inner.saved_frame_rate.store(fps, Ordering::Release);
            inner.set_frame_rate(fps * BOOST_MULTIPLIER);
        }
        inner.invalidate_tick();
    }

    fn invalidate_tick(&self, inner: &Arc<Inner>) {
        if inner.invalidate_tick_count.fetch_add(1, Ordering::AcqRel) + 1 > INVALIDATE_TICK_LIMIT {
            inner.invalidate_stop.store(true, Ordering::Release);
        }
        if inner.invalidate_stop.load(Ordering::Acquire) {
            let saved = inner.saved_frame_rate.swap(0, Ordering::AcqRel);
            if inner.browser_alive() && saved > 0 {
                inner.set_frame_rate(saved);
            }
            inner.invalidate_running.store(false, Ordering::Release);
            return;
        }
        if inner.browser_alive() {
            inner.invalidate_view();
            #[cfg(target_os = "macos")]
            inner.send_external_begin_frame();
        }
        let fps = inner.frame_rate.load(Ordering::Acquire);
        if fps <= 0 {
            inner.invalidate_running.store(false, Ordering::Release);
            return;
        }
        // Tick at 4x display refresh so the compositor gets nudged more
        // often than the boosted output rate (2x) — keeps frame production
        // ahead of the present cadence during a resize.
        let tick_hz = fps * 4;
        let delay_ms = ((1000.0 / tick_hz as f64) + 0.5) as i64;
        let delay_ms = delay_ms.max(1);
        let next = Arc::clone(inner);
        let mut task = TickTask::new(next);
        let _ = post_delayed_task(ThreadId::UI, Some(&mut task), delay_ms);
    }

    fn should_present_paint(&self, inner: &Inner) -> bool {
        let cur_gen = inner.resize_gen.load(Ordering::Acquire);
        let last_gen = inner.last_paint_gen.load(Ordering::Acquire);
        if cur_gen != last_gen {
            inner.last_paint_gen.store(cur_gen, Ordering::Release);
            // Rate-clamp the skip-counter reset. Continuous drag bumps gen
            // many times per second; resetting on every bump would keep
            // wiping the counter before any paint clears the skip threshold.
            let now_ns_val = now_ns();
            let hz = jfn_playback::ingest_driver::jfn_playback_display_hz();
            let period_ns = if hz > 0.0 {
                (1e9 / hz) as i64
            } else {
                16_666_667
            };
            if now_ns_val - inner.last_skip_reset_ns.load(Ordering::Acquire) >= period_ns {
                inner
                    .last_skip_reset_ns
                    .store(now_ns_val, Ordering::Release);
                let fps = inner.frame_rate.load(Ordering::Acquire);
                inner.skip_paints_after_resize.store(1, Ordering::Release);
                inner
                    .pump_paint_count
                    .store(if fps > 0 { 1 + fps } else { 0 }, Ordering::Release);
                inner.paints_since_resize.store(0, Ordering::Release);
            }
        }
        let count = inner.paints_since_resize.fetch_add(1, Ordering::AcqRel) + 1;
        let skip = inner.skip_paints_after_resize.load(Ordering::Acquire);
        let pump = inner.pump_paint_count.load(Ordering::Acquire);
        let present = count > skip;
        if pump > 0 && count == pump {
            // Pumped enough frames — signal stop to host Invalidate loop and
            // renderer's rAF loop. Counter remains past pump so subsequent
            // paints don't re-fire.
            inner.invalidate_stop.store(true, Ordering::Release);
            inner.exec_js("window.__cefStopRaf && window.__cefStopRaf();");
        }
        present
    }
}

pub(crate) fn make(shared_textures: bool) -> Box<dyn PaintScheduler> {
    if shared_textures {
        Box::new(ActivePaintScheduler)
    } else {
        Box::new(PassivePaintScheduler)
    }
}
