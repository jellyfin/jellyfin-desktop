//! Port of `src/mpv/options.h` — hwdec mode list.

use std::ffi::CStr;

pub const HWDEC_DEFAULT: &str = "no";

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
}
