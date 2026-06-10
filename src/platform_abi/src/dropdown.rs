use std::ffi::{c_int, c_void};

use crate::{DisplayBackend, SurfaceHandle};

pub struct JfnPopupRequest {
    pub x: c_int,
    pub y: c_int,
    pub lw: c_int,
    pub lh: c_int,
    pub options: Vec<String>,
    pub initial_highlight: c_int,
    /// Picked option index, `-1` for cancel. Dropping it unfired also cancels.
    pub on_selected: Option<Box<dyn FnOnce(c_int) + Send>>,
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum DropdownStyle {
    /// The backend draws its own menu and fires `on_selected`; CEF's OSR
    /// popup runs invisibly underneath and the pick is replayed into it.
    PlatformMenu,
    Composited,
    JsMenu,
}

pub fn dropdown_style(b: DisplayBackend) -> DropdownStyle {
    match b {
        DisplayBackend::Wayland => DropdownStyle::Composited,
        DisplayBackend::X11 => DropdownStyle::JsMenu,
        DisplayBackend::Windows => DropdownStyle::Composited,
        DisplayBackend::MacOS => DropdownStyle::PlatformMenu,
    }
}

/// Embedded scripts a dropdown mechanism can require. The embedder maps
/// each variant to its own script registry with an exhaustive match, so an
/// unmapped script is a compile error rather than a silent no-inject.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum DropdownScript {
    SelectMenu,
}

/// The menu is rendered by an injected in-page script; the platform hooks
/// stay no-ops and `show` drops the request, which cancels CEF's widget.
pub struct JsMenuDropdown;

impl DropdownBackend for JsMenuDropdown {
    fn scripts(&self) -> &'static [DropdownScript] {
        &[DropdownScript::SelectMenu]
    }
}

pub trait DropdownBackend: Send + Sync {
    fn scripts(&self) -> &'static [DropdownScript] {
        &[]
    }
    fn show(&self, _s: SurfaceHandle, _req: JfnPopupRequest) {}
    fn hide(&self, _s: SurfaceHandle) {}
    fn present(&self, _s: SurfaceHandle, _info: *const c_void, _lw: c_int, _lh: c_int) {}
    fn present_software(
        &self,
        _s: SurfaceHandle,
        _buffer: *const c_void,
        _pw: c_int,
        _ph: c_int,
        _lw: c_int,
        _lh: c_int,
    ) {
    }
}
