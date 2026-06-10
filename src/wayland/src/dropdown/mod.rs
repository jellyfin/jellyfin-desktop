//! Both mechanisms are kept compiled so the style table in
//! `jfn_platform_abi::dropdown` can flip between them.

mod subsurface;
mod xdg_popup;

use jfn_platform_abi::{
    DisplayBackend, DropdownBackend, DropdownStyle, JsMenuDropdown, dropdown_style,
};

use subsurface::SubsurfaceDropdown;
use xdg_popup::XdgPopupDropdown;

pub(crate) fn backend() -> &'static dyn DropdownBackend {
    match dropdown_style(DisplayBackend::Wayland) {
        DropdownStyle::PlatformMenu => &XdgPopupDropdown,
        DropdownStyle::Composited => &SubsurfaceDropdown,
        DropdownStyle::JsMenu => &JsMenuDropdown,
    }
}
