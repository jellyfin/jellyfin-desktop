//! Port of `src/mpv/color.cpp`. Parses any color string mpv emits or accepts
//! (see `third_party/mpv/options/m_option.c:2079-2147`). Returns 0 (black) on
//! malformed input.

/// Parse an mpv-form color into a 24-bit RGB integer (`0x00RRGGBB`).
/// Delegates to `jfn_color::parse_mpv`.
pub fn parse(s: &str) -> u32 {
    jfn_color::parse_mpv(s.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn malformed_returns_zero() {
        assert_eq!(parse(""), 0);
    }

    #[test]
    fn forwards_to_jfn_color() {
        // Trust jfn-color's test suite for the parser itself; just verify
        // wiring with one round-trip.
        assert_eq!(parse("#ff112233"), jfn_color::parse_mpv(b"#ff112233"));
    }
}
