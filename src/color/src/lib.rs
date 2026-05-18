//! CSS- and mpv-form color string parsing. Replaces `src/cef/color.cpp` and
//! `src/mpv/color.cpp`. Both parsers pack the result into a 24-bit RGB integer
//! and use 0 (black) for malformed input — matching the C++ `Color{}` default.

use std::ffi::{CStr, c_char};

fn parse_hex_nibble(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(10 + c - b'a'),
        b'A'..=b'F' => Some(10 + c - b'A'),
        _ => None,
    }
}

fn parse_hex_byte(s: &[u8]) -> Option<u8> {
    if s.len() != 2 {
        return None;
    }
    let hi = parse_hex_nibble(s[0])?;
    let lo = parse_hex_nibble(s[1])?;
    Some((hi << 4) | lo)
}

fn pack(r: u8, g: u8, b: u8) -> u32 {
    ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
}

/// Parse a `<meta name="theme-color">` value: `#RGB` or `#RRGGBB`. Returns 0
/// on malformed input.
pub fn parse_cef(s: &[u8]) -> u32 {
    if s.first() != Some(&b'#') {
        return 0;
    }
    let hex = &s[1..];
    match hex.len() {
        3 => {
            let r = match parse_hex_nibble(hex[0]) { Some(v) => v, None => return 0 };
            let g = match parse_hex_nibble(hex[1]) { Some(v) => v, None => return 0 };
            let b = match parse_hex_nibble(hex[2]) { Some(v) => v, None => return 0 };
            pack(r * 0x11, g * 0x11, b * 0x11)
        }
        6 => {
            let r = match parse_hex_byte(&hex[0..2]) { Some(v) => v, None => return 0 };
            let g = match parse_hex_byte(&hex[2..4]) { Some(v) => v, None => return 0 };
            let b = match parse_hex_byte(&hex[4..6]) { Some(v) => v, None => return 0 };
            pack(r, g, b)
        }
        _ => 0,
    }
}

fn parse_unit_float(s: &[u8]) -> Option<f64> {
    if s.is_empty() {
        return None;
    }
    let txt = std::str::from_utf8(s).ok()?;
    let v: f64 = txt.parse().ok()?;
    if !v.is_finite() || !(0.0..=1.0).contains(&v) {
        return None;
    }
    Some(v)
}

fn scale(v: f64) -> u8 {
    (v * 255.0).round() as u8
}

/// Parse any form mpv emits or accepts (third_party/mpv/options/m_option.c
/// :2079-2147). mpv's `print_color` emits `#AARRGGBB` — alpha first. Does NOT
/// accept CSS `#RGB`. Returns 0 on malformed input.
pub fn parse_mpv(s: &[u8]) -> u32 {
    if s.is_empty() {
        return 0;
    }
    if s[0] == b'#' {
        let hex = &s[1..];
        let rgb = match hex.len() {
            6 => hex,
            8 => &hex[2..],
            _ => return 0,
        };
        let r = match parse_hex_byte(&rgb[0..2]) { Some(v) => v, None => return 0 };
        let g = match parse_hex_byte(&rgb[2..4]) { Some(v) => v, None => return 0 };
        let b = match parse_hex_byte(&rgb[4..6]) { Some(v) => v, None => return 0 };
        return pack(r, g, b);
    }
    if !s.contains(&b'/') {
        return 0;
    }
    let mut comp = [0.0f64; 4];
    let mut n = 0usize;
    for tok in s.split(|&c| c == b'/') {
        if n >= 4 {
            return 0;
        }
        let v = match parse_unit_float(tok) {
            Some(v) => v,
            None => return 0,
        };
        comp[n] = v;
        n += 1;
    }
    if n == 0 {
        return 0;
    }
    // mpv rules: 1 = gray, 2 = gray+alpha, 3 = r/g/b, 4 = r/g/b/a. Alpha
    // is always dropped.
    if n <= 2 {
        let g = scale(comp[0]);
        pack(g, g, g)
    } else {
        pack(scale(comp[0]), scale(comp[1]), scale(comp[2]))
    }
}

unsafe fn cstr_bytes<'a>(s: *const c_char) -> &'a [u8] {
    if s.is_null() {
        return &[];
    }
    unsafe { CStr::from_ptr(s) }.to_bytes()
}

/// # Safety
/// `s` must be NUL-terminated or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_parse_color(s: *const c_char) -> u32 {
    parse_cef(unsafe { cstr_bytes(s) })
}

/// # Safety
/// `s` must be NUL-terminated or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_mpv_parse_color(s: *const c_char) -> u32 {
    parse_mpv(unsafe { cstr_bytes(s) })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cef(s: &str) -> u32 { parse_cef(s.as_bytes()) }
    fn mpv(s: &str) -> u32 { parse_mpv(s.as_bytes()) }

    #[test]
    fn cef_empty_or_non_hash_is_zero() {
        assert_eq!(cef(""), 0);
        assert_eq!(cef("garbage"), 0);
        assert_eq!(cef("blue"), 0);
        assert_eq!(cef("rgb(0,0,255)"), 0);
        assert_eq!(cef("000000"), 0);
        assert_eq!(cef("#"), 0);
    }

    #[test]
    fn cef_rrggbb() {
        assert_eq!(cef("#000000"), 0x000000);
        assert_eq!(cef("#FFFFFF"), 0xFFFFFF);
        assert_eq!(cef("#FF00FF"), 0xFF00FF);
        assert_eq!(cef("#0000FF"), 0x0000FF);
        assert_eq!(cef("#abcdef"), 0xABCDEF);
        assert_eq!(cef("#202020"), 0x202020);
        assert_eq!(cef("#101010"), 0x101010);
    }

    #[test]
    fn cef_rgb_shorthand() {
        assert_eq!(cef("#000"), 0x000000);
        assert_eq!(cef("#fff"), 0xFFFFFF);
        assert_eq!(cef("#abc"), 0xAABBCC);
        assert_eq!(cef("#f0f"), 0xFF00FF);
    }

    #[test]
    fn cef_rejects_mpv_and_weird_lengths() {
        assert_eq!(cef("#FF0000FF"), 0);
        assert_eq!(cef("#0/0/1"), 0);
        assert_eq!(cef("0/0/1"), 0);
        assert_eq!(cef("#ab"), 0);
        assert_eq!(cef("#abcd"), 0);
        assert_eq!(cef("#abcde"), 0);
        assert_eq!(cef("#abcdefg"), 0);
    }

    #[test]
    fn cef_malformed_hex() {
        assert_eq!(cef("#zzz"), 0);
        assert_eq!(cef("#zzzzzz"), 0);
        assert_eq!(cef("#ab cdef"), 0);
        assert_eq!(cef("# 00000"), 0);
    }

    #[test]
    fn mpv_empty() {
        assert_eq!(mpv(""), 0);
    }

    #[test]
    fn mpv_hash_rrggbb() {
        assert_eq!(mpv("#000000"), 0);
        assert_eq!(mpv("#FFFFFF"), 0xFFFFFF);
        assert_eq!(mpv("#abcdef"), 0xABCDEF);
    }

    #[test]
    fn mpv_hash_aarrggbb_drops_alpha() {
        assert_eq!(mpv("#00FFFFFF"), 0xFFFFFF);
        assert_eq!(mpv("#FFAA0000"), 0xAA0000);
        assert_eq!(mpv("#80FF00FF"), 0xFF00FF);
    }

    #[test]
    fn mpv_hash_rejects_other_lengths() {
        assert_eq!(mpv("#"), 0);
        assert_eq!(mpv("#abc"), 0);
        assert_eq!(mpv("#abcde"), 0);
        assert_eq!(mpv("#abcdefg"), 0);
    }

    #[test]
    fn mpv_slash_form() {
        // 3 components: r/g/b
        assert_eq!(mpv("1/0/0"), 0xFF0000);
        assert_eq!(mpv("0/1/0"), 0x00FF00);
        assert_eq!(mpv("0/0/1"), 0x0000FF);
        assert_eq!(mpv("0.5/0.5/0.5"), pack(128, 128, 128));
        // 4 components: r/g/b/a, alpha dropped
        assert_eq!(mpv("1/0/0/0.5"), 0xFF0000);
        // 1 = gray
        assert_eq!(mpv("0.5"), 0); // no slash -> error
        assert_eq!(mpv("0.5/"), 0); // empty tok
    }

    #[test]
    fn mpv_slash_gray_forms() {
        // single value with trailing slash isn't valid (empty 2nd tok)
        // 2 components: gray+alpha (alpha dropped, value used as gray)
        assert_eq!(mpv("1/0"), 0xFFFFFF);
        assert_eq!(mpv("0/1"), 0x000000);
    }

    #[test]
    fn mpv_slash_out_of_range() {
        assert_eq!(mpv("1.5/0/0"), 0);
        assert_eq!(mpv("-0.1/0/0"), 0);
        assert_eq!(mpv("nan/0/0"), 0);
    }

    #[test]
    fn mpv_slash_too_many_fields() {
        assert_eq!(mpv("1/0/0/0/0"), 0);
    }

    #[test]
    fn mpv_no_slash_is_error() {
        assert_eq!(mpv("garbage"), 0);
        assert_eq!(mpv("123"), 0);
    }
}
