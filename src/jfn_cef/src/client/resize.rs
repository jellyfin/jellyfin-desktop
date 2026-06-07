use std::sync::Arc;
use std::sync::atomic::Ordering;

use jfn_playback::ingest_driver::jfn_playback_display_hz;

use super::{Inner, now_ns, platform_ops, tasks};

impl Inner {
    pub(crate) fn set_frame_rate(&self, hz: i32) {
        if hz <= 0 || !self.browser_alive() {
            return;
        }
        self.cef_set_windowless_frame_rate(hz);
        self.current_frame_rate.store(hz, Ordering::Release);
    }

    pub(super) fn apply_pending_resize(self: &Arc<Self>) {
        self.resize_scheduled.store(false, Ordering::Release);
        if !self.browser_alive() {
            return;
        }
        let now = now_ns();
        self.last_was_resized_ns.store(now, Ordering::Release);
        // WasResized retargets the renderer; any stable-size streak (possibly
        // accumulated against the old dims while this apply was pending) must
        // be invalidated.
        self.paint_scheduler.during_resize(self, || {
            self.notify_screen_info_changed();
            self.cef_was_resized();
        });
    }

    pub(super) fn resize(self: &Arc<Self>, w: i32, h: i32, pw: i32, ph: i32) {
        self.width.store(w, Ordering::Release);
        self.height.store(h, Ordering::Release);
        self.physical_w.store(pw, Ordering::Release);
        self.physical_h.store(ph, Ordering::Release);

        // Wayland viewport must update on every configure to avoid stale
        // src/dst — runs immediately.
        let surface = self.surface_ptr();
        if !surface.is_null()
            && let Some(p) = platform_ops::ops()
        {
            p.surface_resize(
                surface,
                platform_ops::SurfaceSize {
                    logical_w: w,
                    logical_h: h,
                    physical_w: pw,
                    physical_h: ph,
                },
            );
        }

        // Defer kick until the browser exists; OnAfterCreated will fire it.
        if !self.browser_alive() {
            return;
        }

        // Debounce the CEF host notify (re-layout) to one display-refresh
        // period. Drag fires many configures per frame; coalescing them
        // saves N-1 wasted re-layouts.
        let now = now_ns();
        let hz = jfn_playback_display_hz();
        let period_ns = if hz > 0.0 {
            (1e9 / hz) as i64
        } else {
            16_666_667
        };
        let last = self.last_was_resized_ns.load(Ordering::Acquire);
        self.paint_scheduler.during_resize(self, || {
            if now - last >= period_ns {
                self.last_was_resized_ns.store(now, Ordering::Release);
                self.notify_screen_info_changed();
                self.cef_was_resized();
                return;
            }
            // Within the debounce window — schedule a single deferred apply if
            // one isn't already pending. Latest width/height get picked up.
            if self
                .resize_scheduled
                .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
            {
                let delay_ms = ((period_ns - (now - last)) / 1_000_000).max(1);
                tasks::post_apply_resize(Arc::clone(self), delay_ms);
            }
        });
    }

    pub(super) fn set_refresh_rate(self: &Arc<Self>, hz: f64) {
        if hz <= 0.0 {
            return;
        }
        let target = hz.ceil() as i32;
        tasks::post_set_refresh(Arc::clone(self), target);
    }

    pub(super) fn apply_set_refresh(&self, target: i32) {
        self.frame_rate.store(target, Ordering::Release);
        // If a nudge-loop boost is active, just update what we'll restore to
        // and let the boost rate keep running. Otherwise apply now.
        if !self.paint_scheduler.refresh_rate_changed(target) {
            self.set_frame_rate(target);
        }
    }
}
