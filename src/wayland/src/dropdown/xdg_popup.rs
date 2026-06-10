use std::ffi::c_int;

use jfn_platform_abi::{DropdownBackend, JfnPopupRequest, SurfaceHandle};

/// Menu drawn as a grabbed xdg_popup, like the context menu. CEF's OSR
/// popup still runs underneath; present/present_software stay no-ops to
/// suppress its pixels.
pub(super) struct XdgPopupDropdown;

impl DropdownBackend for XdgPopupDropdown {
    fn show(&self, _s: SurfaceHandle, req: JfnPopupRequest) {
        let items = req
            .options
            .into_iter()
            .enumerate()
            .map(|(i, label)| jfn_menu::MenuItem {
                id: i as c_int,
                label,
                enabled: true,
                separator: false,
            })
            .collect();
        let cb = req.on_selected.unwrap_or_else(|| Box::new(|_| {}));
        crate::popup::show_highlighted(items, req.x, req.y, req.lw, req.initial_highlight, cb);
    }

    fn hide(&self, _s: SurfaceHandle) {
        crate::popup::hide();
    }
}
