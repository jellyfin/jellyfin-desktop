use std::os::raw::c_void;

use super::{Inner, platform_ops};

impl Inner {
    pub(crate) fn view_size(&self) -> (i32, i32) {
        let v = *self.view.lock();
        (v.logical.width, v.logical.height)
    }

    pub(crate) fn screen_info_values(&self) -> (f64, i32, i32) {
        let v = *self.view.lock();
        (v.scale, v.logical.width, v.logical.height)
    }

    pub(crate) fn view_scale(&self) -> f64 {
        self.view.lock().scale
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
