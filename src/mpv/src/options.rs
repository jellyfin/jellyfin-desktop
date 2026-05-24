//! Port of `src/mpv/options.h` — hwdec mode list.

use std::ffi::{CStr, c_char};

pub const HWDEC_DEFAULT: &str = "no";

static HWDEC_DEFAULT_C: &CStr = c"no";

#[cfg(target_os = "linux")]
static HWDEC_LIST: &[&CStr] = &[c"auto", c"no", c"vaapi", c"nvdec", c"vulkan"];
#[cfg(target_os = "windows")]
static HWDEC_LIST: &[&CStr] = &[c"auto", c"no", c"d3d11va", c"nvdec", c"vulkan"];
#[cfg(target_os = "macos")]
static HWDEC_LIST: &[&CStr] = &[c"auto", c"no", c"videotoolbox", c"vulkan"];

pub fn hwdec_options() -> Vec<&'static str> {
    HWDEC_LIST
        .iter()
        .map(|s| s.to_str().expect("hwdec entries are ASCII"))
        .collect()
}

pub fn is_valid_hwdec(value: &str) -> bool {
    HWDEC_LIST.iter().any(|s| s.to_bytes() == value.as_bytes())
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_mpv_hwdec_default() -> *const c_char {
    HWDEC_DEFAULT_C.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_mpv_hwdec_options_count() -> usize {
    HWDEC_LIST.len()
}

/// Returns the i-th hwdec option as a NUL-terminated C string with static
/// lifetime, or NULL if `i` is out of range.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_mpv_hwdec_options_get(i: usize) -> *const c_char {
    HWDEC_LIST
        .get(i)
        .map(|s| s.as_ptr())
        .unwrap_or(std::ptr::null())
}

/// Returns true if `s` matches one of the platform-specific hwdec options.
/// `s` must be a valid NUL-terminated C string; passing NULL returns false.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_mpv_is_valid_hwdec(s: *const c_char) -> bool {
    if s.is_null() {
        return false;
    }
    let bytes = unsafe { CStr::from_ptr(s) }.to_bytes();
    HWDEC_LIST.iter().any(|opt| opt.to_bytes() == bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_always_valid() {
        assert!(is_valid_hwdec("auto"));
        assert!(is_valid_hwdec("no"));
        assert!(is_valid_hwdec(HWDEC_DEFAULT));
    }

    #[test]
    fn rejects_garbage() {
        assert!(!is_valid_hwdec(""));
        assert!(!is_valid_hwdec("garbage"));
    }

    #[test]
    fn ffi_count_matches_rust_api() {
        assert_eq!(jfn_mpv_hwdec_options_count(), hwdec_options().len());
    }

    #[test]
    fn ffi_get_round_trips() {
        for (i, want) in hwdec_options().iter().enumerate() {
            let ptr = jfn_mpv_hwdec_options_get(i);
            assert!(!ptr.is_null());
            let got = unsafe { CStr::from_ptr(ptr) }.to_str().unwrap();
            assert_eq!(got, *want);
        }
        assert!(jfn_mpv_hwdec_options_get(usize::MAX).is_null());
    }
}
