//! Window-scoped theme color tracker. Ports `src/theme_color.h`.
//!
//! Owns the current theme-color (`<meta name="theme-color">` updates),
//! buffers it until the loading overlay dismisses, and switches to the
//! mpv background color while video is playing so resize letterbox gaps
//! match mpv exactly.
//!
//! The two sink callbacks are installed once at process start:
//!   * `on_set_theme_color(rgb)` — optional; only set when the user has
//!     `titlebarThemeColor` enabled, drives the platform titlebar tint.
//!   * `on_set_bg_hex(c_str)` — required; passes `#RRGGBB` to mpv so its
//!     background matches the chrome during resize.

use std::ffi::c_char;
use std::sync::Mutex;

const DEFAULT_BG_RGB: u32 = 0x101010; // kBgColor

struct ThemeColor {
    on_set_theme_color: Option<unsafe extern "C" fn(u32)>,
    on_set_bg_hex: unsafe extern "C" fn(*const c_char),
    video_bg_rgb: u32,
    current_rgb: u32,
    unlocked: bool,
    video_active: bool,
    last_applied: Option<u32>,
}

impl ThemeColor {
    fn resolved(&self) -> u32 {
        if self.video_active {
            self.video_bg_rgb
        } else {
            self.current_rgb
        }
    }

    fn apply(&mut self) {
        let rgb = self.resolved();
        if self.last_applied == Some(rgb) {
            return;
        }
        self.last_applied = Some(rgb);
        if let Some(f) = self.on_set_theme_color {
            unsafe { f(rgb) };
        }
        let mut hex = [0u8; 8];
        format_hex_rgb(rgb, &mut hex);
        unsafe { (self.on_set_bg_hex)(hex.as_ptr() as *const c_char) };
    }
}

fn format_hex_rgb(rgb: u32, out: &mut [u8; 8]) {
    out[0] = b'#';
    for i in 0..6 {
        let nibble = ((rgb >> (20 - i * 4)) & 0xF) as u8;
        out[1 + i] = if nibble < 10 {
            b'0' + nibble
        } else {
            b'a' + (nibble - 10)
        };
    }
    out[7] = 0;
}

static INSTANCE: Mutex<Option<ThemeColor>> = Mutex::new(None);

/// Initialise the process-wide theme color singleton. Calling a second time
/// replaces the previous state — the new sink callbacks take effect on the
/// next `apply()`.
///
/// # Safety
/// Callbacks must be valid for the lifetime of the process.
pub unsafe extern "C" fn jfn_theme_color_init(
    on_set_theme_color: Option<unsafe extern "C" fn(u32)>,
    on_set_bg_hex: Option<unsafe extern "C" fn(*const c_char)>,
) {
    let Some(on_set_bg_hex) = on_set_bg_hex else {
        return;
    };
    let mut tc = ThemeColor {
        on_set_theme_color,
        on_set_bg_hex,
        video_bg_rgb: DEFAULT_BG_RGB,
        current_rgb: DEFAULT_BG_RGB,
        unlocked: false,
        video_active: false,
        last_applied: None,
    };
    tc.apply();
    *INSTANCE.lock().unwrap() = Some(tc);
}

pub extern "C" fn jfn_theme_color_set_video_bg(rgb: u32) {
    let mut g = INSTANCE.lock().unwrap();
    if let Some(t) = g.as_mut() {
        t.video_bg_rgb = rgb;
        if t.video_active && t.unlocked {
            t.apply();
        }
    }
}

pub extern "C" fn jfn_theme_color_on_color(rgb: u32) {
    let mut g = INSTANCE.lock().unwrap();
    if let Some(t) = g.as_mut() {
        t.current_rgb = rgb;
        if t.unlocked {
            t.apply();
        }
    }
}

pub extern "C" fn jfn_theme_color_on_overlay_dismissed() {
    let mut g = INSTANCE.lock().unwrap();
    if let Some(t) = g.as_mut() {
        t.unlocked = true;
        t.apply();
    }
}

pub extern "C" fn jfn_theme_color_set_video_mode(active: bool) {
    let mut g = INSTANCE.lock().unwrap();
    if let Some(t) = g.as_mut() {
        if t.video_active == active {
            return;
        }
        t.video_active = active;
        if t.unlocked {
            t.apply();
        }
    }
}

pub extern "C" fn jfn_theme_color_shutdown() {
    *INSTANCE.lock().unwrap() = None;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_format() {
        let mut buf = [0u8; 8];
        format_hex_rgb(0x101010, &mut buf);
        assert_eq!(&buf[..7], b"#101010");
        format_hex_rgb(0xabcdef, &mut buf);
        assert_eq!(&buf[..7], b"#abcdef");
        format_hex_rgb(0x000000, &mut buf);
        assert_eq!(&buf[..7], b"#000000");
    }
}
