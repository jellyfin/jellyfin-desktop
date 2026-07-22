use std::os::raw::c_void;
use std::sync::atomic::Ordering;

use super::{Inner, platform_ops};

impl Inner {
    pub(crate) fn view_size(&self) -> (i32, i32) {
        (
            self.width.load(Ordering::Acquire),
            self.height.load(Ordering::Acquire),
        )
    }

    pub(crate) fn screen_info_values(&self) -> (f32, i32, i32) {
        let w = self.width.load(Ordering::Acquire);
        let h = self.height.load(Ordering::Acquire);
        let pw = self.physical_w.load(Ordering::Acquire);
        let ph = self.physical_h.load(Ordering::Acquire);
        let scale = surface_scale(w, h, pw, ph);
        (scale, w, h)
    }

    pub(crate) fn on_paint(
        &self,
        is_popup: bool,
        dirty: *const platform_ops::JfnRect,
        n: usize,
        buffer: *const c_void,
        w: i32,
        h: i32,
    ) {
        let surface = self.surface_ptr();
        if surface.is_null() {
            return;
        }
        if is_popup {
            let (pw, ph) = self.popup_rect();
            self.dropdown
                .present_software(surface, buffer, w, h, pw, ph);
            return;
        }
        let Some(p) = platform_ops::ops() else { return };
        if !self.should_present_paint() {
            return;
        }
        let dirty: &[platform_ops::JfnRect] = if dirty.is_null() || n == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(dirty, n) }
        };
        p.surface_present_software(surface, dirty, buffer, w, h);
    }

    pub(crate) fn on_accelerated_paint(&self, is_popup: bool, info: *const c_void) {
        let surface = self.surface_ptr();
        if surface.is_null() || info.is_null() {
            return;
        }
        if is_popup {
            let (pw, ph) = self.popup_rect();
            self.dropdown.present(surface, info, pw, ph);
            return;
        }
        let Some(p) = platform_ops::ops() else { return };
        if !self.should_present_paint() {
            return;
        }
        p.surface_present(surface, info);
    }

    fn should_present_paint(&self) -> bool {
        self.paint_scheduler.should_present_paint(self)
    }
}

fn surface_scale(w: i32, h: i32, pw: i32, ph: i32) -> f32 {
    if w <= 0 || h <= 0 || pw <= 0 || ph <= 0 {
        return 1.0;
    }
    (pw as f32 / w as f32).max(ph as f32 / h as f32)
}

#[cfg(test)]
mod tests {
    use super::surface_scale;

    #[test]
    fn scale_covers_both_physical_axes() {
        let scale = surface_scale(1536, 800, 3840, 1999);
        assert!(1536.0 * scale >= 3840.0);
        assert!(800.0 * scale >= 1999.0);
    }
}
