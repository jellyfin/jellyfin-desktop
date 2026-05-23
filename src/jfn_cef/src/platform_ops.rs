//! Thin re-export shim over [`jfn_platform_abi`]. Historically this crate
//! held a C-ABI mirror of a vtable populated by C++ thunks; the platform
//! layer is now an all-Rust trait, so callers just dispatch through it.

pub use jfn_platform_abi::{JfnPopupRequest, JfnRect, Platform};

/// Returns the installed platform backend, or `None` if no backend has
/// been installed yet (e.g. early CEF helper-process boot before
/// `jfn_app_main` runs).
pub fn ops() -> Option<&'static dyn Platform> {
    jfn_platform_abi::try_get()
}
