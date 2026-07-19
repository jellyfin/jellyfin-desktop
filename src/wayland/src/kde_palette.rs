//! KDE/KWin per-window titlebar color support.

use parking_lot::Mutex;
use std::ffi::CString;
use std::fs;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;

const COLOR_SCHEME_TEMPLATE: &str = include_str!("kde_palette_template.ini");

struct PaletteState {
    colors_dir: PathBuf,
    current_path: Option<CString>,
}

static STATE: Mutex<Option<PaletteState>> = Mutex::new(None);

fn write_color_scheme(r: u8, g: u8, b: u8, path: &std::path::Path) -> std::io::Result<()> {
    let bg = format!("{},{},{}", r, g, b);

    // BT.709 luminance — choose readable foreground.
    let lum =
        0.2126 * (r as f64 / 255.0) + 0.7152 * (g as f64 / 255.0) + 0.0722 * (b as f64 / 255.0);
    let active_fg = if lum < 0.5 { "252,252,252" } else { "35,38,41" };
    let inactive_fg = if lum < 0.5 { "126,126,126" } else { "35,38,41" };

    let content = COLOR_SCHEME_TEMPLATE
        .replace("%HEADER_BG%", &bg)
        .replace("%INACTIVE_BG%", &bg)
        .replace("%ACTIVE_FG%", active_fg)
        .replace("%INACTIVE_FG%", inactive_fg);

    fs::write(path, content)
}

fn make_colors_dir() -> Option<PathBuf> {
    let runtime = match std::env::var_os("XDG_RUNTIME_DIR") {
        Some(s) if !s.is_empty() => s,
        _ => return None,
    };
    let mut dir = PathBuf::from(runtime);
    dir.push("jellium-desktop");
    if let Err(e) = fs::create_dir_all(&dir) {
        tracing::warn!("kde_palette: mkdir {} failed: {}", dir.display(), e);
        return None;
    }
    let _ = fs::set_permissions(&dir, fs::Permissions::from_mode(0o700));
    Some(dir)
}

pub(crate) fn init() -> bool {
    if STATE.lock().is_some() {
        return true;
    }
    let Some(colors_dir) = make_colors_dir() else {
        return false;
    };
    *STATE.lock() = Some(PaletteState {
        colors_dir,
        current_path: None,
    });
    true
}

pub(crate) fn set_color(r: u8, g: u8, b: u8, hex: &std::ffi::CStr) {
    let hex_str = match hex.to_str() {
        Ok(s) if s.len() == 7 && s.starts_with('#') => &s[1..],
        _ => return,
    };

    let mut guard = STATE.lock();
    let state = match guard.as_mut() {
        Some(s) => s,
        None => return,
    };

    let mut new_path = state.colors_dir.clone();
    new_path.push(format!("JelliumDesktop-{}.colors", hex_str));

    let new_path_c = match CString::new(new_path.as_os_str().as_encoded_bytes()) {
        Ok(c) => c,
        Err(_) => return,
    };
    if state.current_path.as_ref() == Some(&new_path_c) {
        return;
    }

    if let Err(e) = write_color_scheme(r, g, b, &new_path) {
        tracing::warn!("kde_palette: write {} failed: {}", new_path.display(), e);
        return;
    }

    if let Some(old) = state.current_path.take() {
        let old_path = std::path::Path::new(std::ffi::OsStr::from_bytes(old.as_bytes()));
        let _ = fs::remove_file(old_path);
    }

    crate::root_window::set_titlebar_palette(&new_path);
    state.current_path = Some(new_path_c);
}

pub(crate) fn post_window_cleanup() {
    let mut guard = STATE.lock();
    let state = match guard.as_mut() {
        Some(s) => s,
        None => return,
    };
    if let Some(old) = state.current_path.take() {
        let old_path = std::path::Path::new(std::ffi::OsStr::from_bytes(old.as_bytes()));
        let _ = fs::remove_file(old_path);
    }
}
