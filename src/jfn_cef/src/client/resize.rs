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
        self.paint_scheduler.during_resize(self, || {
            self.notify_screen_info_changed();
            self.cef_was_resized();
        });
    }

    pub(super) fn resize(self: &Arc<Self>, w: i32, h: i32, pw: i32, ph: i32, scale: f64) {
        let size = platform_ops::SurfaceSize {
            logical: jfn_platform_abi::LogicalSize::new(w, h),
            physical: jfn_platform_abi::PhysicalSize::new(pw, ph),
            scale: jfn_platform_abi::scale_or_one(scale),
        };
        {
            let mut v = self.view.lock();
            v.logical = size.logical;
            v.physical = size.physical;
            v.scale = size.scale;
        }

        // Wayland viewport must update on every configure (not debounced) or
        // src/dst go stale.
        let surface = self.surface_ptr();
        if !surface.is_null()
            && let Some(p) = platform_ops::ops()
        {
            p.surface_resize(surface, size);
        }

        if !self.browser_alive() {
            return;
        }

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
        if !self.paint_scheduler.refresh_rate_changed(target) {
            self.set_frame_rate(target);
        }
    }
}
