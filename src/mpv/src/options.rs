//! Port of `src/mpv/options.h` — hwdec mode list.

pub const HWDEC_DEFAULT: &str = "no";

pub fn hwdec_options() -> Vec<&'static str> {
    let mut out: Vec<&'static str> = vec!["auto", "no"];
    #[cfg(target_os = "linux")]
    out.extend_from_slice(&["vaapi", "nvdec", "vulkan"]);
    #[cfg(target_os = "windows")]
    out.extend_from_slice(&["d3d11va", "nvdec", "vulkan"]);
    #[cfg(target_os = "macos")]
    out.extend_from_slice(&["videotoolbox", "vulkan"]);
    out
}

pub fn is_valid_hwdec(value: &str) -> bool {
    hwdec_options().iter().any(|o| *o == value)
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
