// DropdownBackend carries raw-pointer args (dmabuf info, paint buffers) from
// CEF; the impl forwards them to unsafe FFI fns.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::ffi::{c_int, c_void};

use jfn_platform_abi::{DropdownBackend, JfnPopupRequest, SurfaceHandle};

use crate::make_platform::to_dmabuf_frame;
use crate::wl_ops;

pub(super) struct SubsurfaceDropdown;

impl DropdownBackend for SubsurfaceDropdown {
    fn show(&self, s: SurfaceHandle, req: JfnPopupRequest) {
        wl_ops::popup_show(
            s as *mut crate::wl_state::PlatformSurface,
            req.x,
            req.y,
            req.lw,
            req.lh,
        );
    }

    fn hide(&self, s: SurfaceHandle) {
        wl_ops::popup_hide(s as *mut crate::wl_state::PlatformSurface);
    }

    fn present(&self, s: SurfaceHandle, info: *const c_void, lw: c_int, lh: c_int) {
        let Some(frame) = (unsafe { to_dmabuf_frame(info) }) else {
            return;
        };
        wl_ops::popup_present(s as *mut crate::wl_state::PlatformSurface, &frame, lw, lh);
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
        if buffer.is_null() || pw <= 0 || ph <= 0 {
            return;
        }
        let len = (pw as usize)
            .checked_mul(ph as usize)
            .and_then(|n| n.checked_mul(4));
        let Some(len) = len else { return };
        let pixels = unsafe { std::slice::from_raw_parts(buffer as *const u8, len) };
        wl_ops::popup_present_software(
            s as *mut crate::wl_state::PlatformSurface,
            pixels,
            pw,
            ph,
            lw,
            lh,
        );
    }
}
