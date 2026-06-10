use std::ffi::{c_int, c_void};

use jfn_platform_abi::{DropdownBackend, JfnPopupRequest, SurfaceHandle};

use crate::compositor::{
    win_popup_hide, win_popup_present, win_popup_present_software, win_popup_show,
};

pub(crate) struct CompositorDropdown;

impl DropdownBackend for CompositorDropdown {
    fn show(&self, s: SurfaceHandle, req: JfnPopupRequest) {
        win_popup_show(s, req.x, req.y);
    }

    fn hide(&self, s: SurfaceHandle) {
        win_popup_hide(s);
    }

    fn present(&self, s: SurfaceHandle, info: *const c_void, lw: c_int, lh: c_int) {
        win_popup_present(s, info, lw, lh);
    }

    fn present_software(
        &self,
        s: SurfaceHandle,
        buffer: *const c_void,
        pw: c_int,
        ph: c_int,
        lw: c_int,
        lh: c_int,
    ) {
        win_popup_present_software(s, buffer, pw, ph, lw, lh);
    }
}
