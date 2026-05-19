//! KDE/KWin per-window titlebar color support.
//!
//! Owns the on-disk color-scheme files written under
//! `$XDG_RUNTIME_DIR/jellyfin-desktop/` that KWin reads when the C++ side
//! calls `org_kde_kwin_server_decoration_palette_set_palette()`. The Wayland
//! protocol bindings themselves still live in `src/platform/wayland.cpp` —
//! this module just produces the file the protocol points at.
//!
//! Lifecycle (driven from C++ wayland.cpp):
//!   1. `jfn_wl_kde_palette_init()` — make `colors_dir`
//!   2. `jfn_wl_kde_palette_write(rgb)` — write a scheme file, return its
//!      path. Returns NULL when the requested color matches the current one
//!      (no protocol call needed) or when writing fails.
//!   3. `jfn_wl_kde_palette_post_window_cleanup()` — unlink the active
//!      scheme file after the window is gone.

use std::ffi::{CString, c_char};
use std::fs;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::sync::Mutex;

const COLOR_SCHEME_TEMPLATE: &str = include_str!("kde_palette_template.ini");

struct PaletteState {
    colors_dir: PathBuf,
    current_path: Option<CString>,
}

static STATE: Mutex<Option<PaletteState>> = Mutex::new(None);

fn write_color_scheme(r: u8, g: u8, b: u8, path: &std::path::Path) -> std::io::Result<()> {
    let bg = format!("{},{},{}", r, g, b);

    // BT.709 luminance — choose readable foreground.
    let lum = 0.2126 * (r as f64 / 255.0)
        + 0.7152 * (g as f64 / 255.0)
        + 0.0722 * (b as f64 / 255.0);
    let active_fg = if lum < 0.5 { "252,252,252" } else { "35,38,41" };
    let inactive_fg = if lum < 0.5 { "126,126,126" } else { "35,38,41" };

    let content = COLOR_SCHEME_TEMPLATE
        .replace("%HEADER_BG%", &bg)
        .replace("%INACTIVE_BG%", &bg)
        .replace("%ACTIVE_FG%", active_fg)
        .replace("%INACTIVE_FG%", inactive_fg);

    fs::write(path, content)
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_wl_kde_palette_init() -> bool {
    let runtime = match std::env::var_os("XDG_RUNTIME_DIR") {
        Some(s) if !s.is_empty() => s,
        _ => return false,
    };
    let mut dir = PathBuf::from(runtime);
    dir.push("jellyfin-desktop");
    if let Err(e) = fs::create_dir_all(&dir) {
        log::warn!("kde_palette: mkdir {} failed: {}", dir.display(), e);
        return false;
    }
    let _ = fs::set_permissions(&dir, fs::Permissions::from_mode(0o700));

    *STATE.lock().unwrap() = Some(PaletteState {
        colors_dir: dir,
        current_path: None,
    });
    true
}

/// Write a color-scheme file for the given color and return its path (UTF-8
/// NUL-terminated, valid until the next call). Returns NULL when:
///   - palette state not initialised
///   - the requested color matches the previously written one
///   - the write failed
///
/// `hex` is a 7-byte NUL-terminated string of the form "#RRGGBB" used only
/// for the filename. Caller (C++) must pass exactly the `Color::hex` field.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_wl_kde_palette_write(
    r: u8,
    g: u8,
    b: u8,
    hex: *const c_char,
) -> *const c_char {
    if hex.is_null() {
        return std::ptr::null();
    }
    let hex_str = match unsafe { std::ffi::CStr::from_ptr(hex) }.to_str() {
        Ok(s) if s.len() == 7 && s.starts_with('#') => &s[1..],
        _ => return std::ptr::null(),
    };

    let mut guard = STATE.lock().unwrap();
    let state = match guard.as_mut() {
        Some(s) => s,
        None => return std::ptr::null(),
    };

    let mut new_path = state.colors_dir.clone();
    new_path.push(format!("JellyfinDesktop-{}.colors", hex_str));

    let new_path_c = match CString::new(new_path.as_os_str().as_encoded_bytes()) {
        Ok(c) => c,
        Err(_) => return std::ptr::null(),
    };
    if state.current_path.as_ref() == Some(&new_path_c) {
        return std::ptr::null();
    }

    if let Err(e) = write_color_scheme(r, g, b, &new_path) {
        log::warn!("kde_palette: write {} failed: {}", new_path.display(), e);
        return std::ptr::null();
    }

    if let Some(old) = state.current_path.take() {
        let old_bytes = old.as_bytes();
        let old_path = std::path::Path::new(std::ffi::OsStr::from_bytes(old_bytes));
        let _ = fs::remove_file(old_path);
    }
    state.current_path = Some(new_path_c);
    state.current_path.as_ref().unwrap().as_ptr()
}



#[unsafe(no_mangle)]
pub extern "C" fn jfn_wl_kde_palette_post_window_cleanup() {
    let mut guard = STATE.lock().unwrap();
    let state = match guard.as_mut() {
        Some(s) => s,
        None => return,
    };
    if let Some(old) = state.current_path.take() {
        let old_bytes = old.as_bytes();
        let old_path = std::path::Path::new(std::ffi::OsStr::from_bytes(old_bytes));
        let _ = fs::remove_file(old_path);
    }
}

