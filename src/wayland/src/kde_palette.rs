//! KDE/KWin per-window titlebar color support.
//!
//! Owns both the on-disk color-scheme files written under
//! `$XDG_RUNTIME_DIR/jellyfin-desktop/` *and* the wire-side
//! `org_kde_kwin_server_decoration_palette_manager` /
//! `org_kde_kwin_server_decoration_palette` protocol bindings. KWin reads
//! the file referenced by the most recent `set_palette` request and applies
//! the colors to the server-side decoration.
//!
//! The protocol bindings are generated from the vendored
//! `protocols/server-decoration-palette.xml` via `build.rs`; there is no
//! upstream Rust crate for this KDE-specific protocol.
//!
//! Lifecycle (driven from C++ wayland.cpp):
//!   1. `jfn_wl_kde_palette_attach(display, parent)` — opens a short-lived
//!      Wayland registry pass on the supplied (mpv-owned) display, binds
//!      the palette manager, creates the per-window palette object, and
//!      seeds the colors directory. Returns `false` if the compositor is
//!      not KWin or the protocol is not advertised.
//!   2. `jfn_wl_kde_palette_set_color(r,g,b,hex)` — writes a scheme file
//!      and dispatches `set_palette(path)` on the palette object. Safe to
//!      call from any thread.
//!   3. `jfn_wl_kde_palette_post_window_cleanup()` — unlinks the active
//!      scheme file after mpv has destroyed the parent surface. The
//!      palette object itself is dropped atomically by KWin when the
//!      window goes away.

use std::ffi::{CString, c_char, c_void};
use std::fs;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::sync::Mutex;

use wayland_backend::client::{Backend, ObjectId};
use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::{wl_registry, wl_surface::WlSurface};
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle};

mod proto {
    #![allow(dead_code, non_camel_case_types, unused_unsafe, unused_variables)]
    #![allow(non_upper_case_globals, non_snake_case)]
    #![allow(missing_docs, clippy::all)]
    use wayland_client;
    use wayland_client::protocol::*;
    pub mod __interfaces {
        use wayland_client::protocol::__interfaces::*;
        wayland_scanner::generate_interfaces!("protocols/server-decoration-palette.xml");
    }
    use self::__interfaces::*;
    wayland_scanner::generate_client_code!("protocols/server-decoration-palette.xml");
}

use proto::org_kde_kwin_server_decoration_palette::OrgKdeKwinServerDecorationPalette;
use proto::org_kde_kwin_server_decoration_palette_manager::OrgKdeKwinServerDecorationPaletteManager;

const COLOR_SCHEME_TEMPLATE: &str = include_str!("kde_palette_template.ini");

struct PaletteState {
    conn: Connection,
    palette: OrgKdeKwinServerDecorationPalette,
    colors_dir: PathBuf,
    current_path: Option<CString>,
}

static STATE: Mutex<Option<PaletteState>> = Mutex::new(None);

// Dispatch sinks. The palette protocol has no client-side events, and we
// only do a single roundtrip against the registry to discover the manager
// global. No state needs to persist across dispatch.
struct RegistrySink;

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for RegistrySink {
    fn event(
        _: &mut Self,
        _: &wl_registry::WlRegistry,
        _: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<OrgKdeKwinServerDecorationPaletteManager, ()> for RegistrySink {
    fn event(
        _: &mut Self,
        _: &OrgKdeKwinServerDecorationPaletteManager,
        _: <OrgKdeKwinServerDecorationPaletteManager as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<OrgKdeKwinServerDecorationPalette, ()> for RegistrySink {
    fn event(
        _: &mut Self,
        _: &OrgKdeKwinServerDecorationPalette,
        _: <OrgKdeKwinServerDecorationPalette as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

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
    dir.push("jellyfin-desktop");
    if let Err(e) = fs::create_dir_all(&dir) {
        log::warn!("kde_palette: mkdir {} failed: {}", dir.display(), e);
        return None;
    }
    let _ = fs::set_permissions(&dir, fs::Permissions::from_mode(0o700));
    Some(dir)
}

/// Attach to the mpv-owned display, discover the KDE palette manager, and
/// create a palette object bound to `parent_surface`. Returns `false` if
/// the protocol is not advertised (non-KWin compositor) or any wire step
/// fails. Safe to call once during wl_init.
///
/// SAFETY: `display` must be a live `*mut wl_display`; `parent_surface`
/// must be a live `*mut wl_proxy` referring to a `wl_surface` on it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_wl_kde_palette_attach(
    display: *mut c_void,
    parent_surface: *mut c_void,
) -> bool {
    if display.is_null() || parent_surface.is_null() {
        return false;
    }
    if STATE.lock().unwrap().is_some() {
        log::warn!("kde_palette: attach called twice");
        return false;
    }

    let backend = unsafe { Backend::from_foreign_display(display.cast()) };
    let conn = Connection::from_backend(backend);

    let (globals, mut queue) = match registry_queue_init::<RegistrySink>(&conn) {
        Ok(p) => p,
        Err(e) => {
            log::warn!("kde_palette: registry init: {e}");
            return false;
        }
    };
    let qh = queue.handle();

    let manager: OrgKdeKwinServerDecorationPaletteManager = match globals.bind(&qh, 1..=1, ()) {
        Ok(m) => m,
        Err(_) => {
            log::info!("kde_palette: protocol not advertised; skipping titlebar colors");
            return false;
        }
    };

    // Wrap mpv's parent surface as a foreign Proxy.
    let parent_id =
        match unsafe { ObjectId::from_ptr(WlSurface::interface(), parent_surface.cast()) } {
            Ok(id) => id,
            Err(_) => {
                log::warn!("kde_palette: parent surface interface mismatch");
                return false;
            }
        };
    let parent = match WlSurface::from_id(&conn, parent_id) {
        Ok(p) => p,
        Err(_) => {
            log::warn!("kde_palette: parent surface from_id failed");
            return false;
        }
    };

    let palette = manager.create(&parent, &qh, ());
    let _ = conn.flush();
    let mut sink = RegistrySink;
    let _ = queue.roundtrip(&mut sink);
    drop(manager);

    let colors_dir = match make_colors_dir() {
        Some(d) => d,
        None => return false,
    };

    *STATE.lock().unwrap() = Some(PaletteState {
        conn,
        palette,
        colors_dir,
        current_path: None,
    });

    log::info!("KDE decoration palette ready");
    true
}

/// Set the current titlebar color. Writes a scheme file (idempotent — no
/// wire traffic if the color hasn't changed) and dispatches `set_palette`
/// pointing at it. `hex` is `Color::hex` — a 7-byte NUL-terminated
/// `"#RRGGBB"` used only for the filename.
///
/// SAFETY: `hex` must be a valid NUL-terminated UTF-8 pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_wl_kde_palette_set_color(r: u8, g: u8, b: u8, hex: *const c_char) {
    if hex.is_null() {
        return;
    }
    let hex_str = match unsafe { std::ffi::CStr::from_ptr(hex) }.to_str() {
        Ok(s) if s.len() == 7 && s.starts_with('#') => &s[1..],
        _ => return,
    };

    let mut guard = STATE.lock().unwrap();
    let state = match guard.as_mut() {
        Some(s) => s,
        None => return,
    };

    let mut new_path = state.colors_dir.clone();
    new_path.push(format!("JellyfinDesktop-{}.colors", hex_str));

    let new_path_c = match CString::new(new_path.as_os_str().as_encoded_bytes()) {
        Ok(c) => c,
        Err(_) => return,
    };
    if state.current_path.as_ref() == Some(&new_path_c) {
        return;
    }

    if let Err(e) = write_color_scheme(r, g, b, &new_path) {
        log::warn!("kde_palette: write {} failed: {}", new_path.display(), e);
        return;
    }

    if let Some(old) = state.current_path.take() {
        let old_path = std::path::Path::new(std::ffi::OsStr::from_bytes(old.as_bytes()));
        let _ = fs::remove_file(old_path);
    }

    let path_str = new_path.to_string_lossy().into_owned();
    state.palette.set_palette(path_str);
    let _ = state.conn.flush();
    state.current_path = Some(new_path_c);
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_wl_kde_palette_post_window_cleanup() {
    let mut guard = STATE.lock().unwrap();
    let state = match guard.as_mut() {
        Some(s) => s,
        None => return,
    };
    if let Some(old) = state.current_path.take() {
        let old_path = std::path::Path::new(std::ffi::OsStr::from_bytes(old.as_bytes()));
        let _ = fs::remove_file(old_path);
    }
}
