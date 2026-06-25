//! CEF's Alloy OSR popup renders `<select>` hover/selection highlights as
//! opaque black on macOS, so we run its popup invisibly and present a native
//! NSMenu in its place.

use std::ffi::c_int;

use jfn_platform_abi::{DropdownBackend, JfnPopupRequest, SurfaceHandle};

use crate::ns_menu::{MenuEntry, MenuSpec, present_on_main};

pub(crate) struct NsMenuDropdown;

impl DropdownBackend for NsMenuDropdown {
    fn show(&self, _s: SurfaceHandle, req: JfnPopupRequest) {
        if req.options.is_empty() {
            return;
        }
        let entries = req
            .options
            .into_iter()
            .enumerate()
            .map(|(i, title)| MenuEntry {
                title,
                tag: i as c_int,
                enabled: true,
                separator: false,
                checked: i as c_int == req.initial_highlight,
            })
            .collect();
        let spec = MenuSpec {
            entries,
            x: req.x,
            y: req.y,
            positioning_tag: (req.initial_highlight >= 0).then_some(req.initial_highlight),
            min_width: Some(req.lw),
        };
        present_on_main(spec, req.on_selected);
    }
}
