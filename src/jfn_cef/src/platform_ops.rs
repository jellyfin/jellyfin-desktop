//! Thin re-export shim over [`jfn_platform_abi`].

pub use jfn_platform_abi::{
    DisplayBackend, JfnContextMenuRequest, JfnMenuItem, JfnPopupRequest, JfnRect, Platform,
    SurfaceSize,
};

/// Returns the installed platform backend, or `None` if no backend has
/// been installed yet (e.g. early CEF helper-process boot before
/// `jfn_app_main` runs).
pub fn ops() -> Option<&'static dyn Platform> {
    jfn_platform_abi::try_get()
}
