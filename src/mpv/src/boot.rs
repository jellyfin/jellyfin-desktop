//! End-to-end mpv handle bring-up: create, apply defaults + per-arg
//! options, initialize, and set log level — exposed as a single
//! `jfn_mpv_handle_init` entry point.
//!
//! Rust owns the lifetime: a process-global slot retains the
//! [`Handle`], and `jfn_mpv_handle_terminate` drops it, calling
//! `mpv_terminate_destroy` via [`Handle::Drop`].

use parking_lot::Mutex;
use std::ffi::CStr;
use std::os::raw::c_char;
use std::ptr;
use std::sync::OnceLock;

use crate::handle::Handle;
use crate::sys;

/// Display backend in use for this process.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum DisplayBackend {
    Wayland = 0,
    X11 = 1,
    Other = 2,
}

impl DisplayBackend {
    fn from_raw(v: u8) -> Self {
        match v {
            0 => Self::Wayland,
            1 => Self::X11,
            _ => Self::Other,
        }
    }
}

/// Boot-time configuration handed to `jfn_mpv_handle_init`. Every
/// option applied between `mpv_create` and `mpv_initialize` lives here.
/// All string fields are NUL-terminated UTF-8 or
/// null; non-null pointers must remain valid for the duration of the
/// init call only (Rust copies what it needs).
#[repr(C)]
pub struct JfnMpvBoot {
    pub display_backend: u8,
    /// Hardware-decoding mode, e.g. `"auto"`, `"no"`, `"vaapi"`.
    pub hwdec: *const c_char,
    pub user_agent: *const c_char,
    /// Optional `--audio-spdif` codecs (e.g. `"ac3,dts-hd,eac3,truehd"`).
    pub audio_passthrough: *const c_char,
    pub audio_exclusive: bool,
    pub audio_channels: *const c_char,
    /// Optional `<W>x<H>[+x+y]` geometry string from saved settings.
    pub geometry: *const c_char,
    pub force_window_position: bool,
    pub window_maximized_at_boot: bool,
    /// libmpv log-message subscription level (`"no"`, `"error"`,
    /// `"warn"`, `"info"`, `"v"`, `"debug"`, `"trace"`).
    pub mpv_log_level: *const c_char,
    /// When set on Wayland, suppress mpv's server-side decoration request so
    /// the app's own client-side decorations don't stack under a compositor
    /// titlebar (e.g. KDE). No effect on X11 (WM draws decorations).
    pub client_side_decorations: bool,
}

/// Owns the Handle for the rest of the process. `mpv_terminate_destroy`
/// fires when the slot is taken via [`jfn_mpv_handle_terminate`].
fn handle_slot() -> &'static Mutex<Option<Handle>> {
    static SLOT: OnceLock<Mutex<Option<Handle>>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(None))
}

unsafe fn cstr_opt(p: *const c_char) -> Option<String> {
    if p.is_null() {
        return None;
    }
    Some(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
}

/// Set a string option, tolerating options absent from the linked libmpv build
/// (e.g. wayland-app-id on the windows build). Warns and continues on
/// MPV_ERROR_OPTION_NOT_FOUND; propagates other errors after logging the name.
fn set_option_or_skip(handle: &Handle, name: &str, value: &str) -> crate::error::Result<()> {
    match handle.set_option_string(name, value) {
        Ok(()) => Ok(()),
        Err(e) if e.code == sys::mpv_error::MPV_ERROR_OPTION_NOT_FOUND.0 => {
            tracing::warn!(target: "mpv", "option {} not supported by this libmpv build; skipping", name);
            Ok(())
        }
        Err(e) => {
            tracing::error!(target: "mpv", "set_option_string({}={}) failed: {:?}", name, value, e);
            Err(e)
        }
    }
}

/// Flag variant of [`set_option_or_skip`].
fn set_option_flag_or_skip(handle: &Handle, name: &str, value: bool) -> crate::error::Result<()> {
    match handle.set_option_flag(name, value) {
        Ok(()) => Ok(()),
        Err(e) if e.code == sys::mpv_error::MPV_ERROR_OPTION_NOT_FOUND.0 => {
            tracing::warn!(target: "mpv", "option {} not supported by this libmpv build; skipping", name);
            Ok(())
        }
        Err(e) => {
            tracing::error!(target: "mpv", "set_option_flag({}={}) failed: {:?}", name, value, e);
            Err(e)
        }
    }
}

fn apply_defaults(
    handle: &Handle,
    display: DisplayBackend,
    client_side_decorations: bool,
) -> crate::error::Result<()> {
    let set = |name: &str, value: &str| set_option_or_skip(handle, name, value);

    // OSD/OSC off — CEF overlay handles all UI.
    set("osd-level", "0")?;
    set("osc", "no")?;
    set("display-tags", "")?;

    // Track selection is owned by Jellyfin. Disable mpv's heuristic
    // so unspecified tracks stay disabled instead of being auto-picked
    // by language / default-flag / codec scoring.
    set("track-auto-selection", "no")?;

    // Input: we own all devices and route through CEF.
    set("input-default-bindings", "no")?;
    set("input-vo-keyboard", "no")?;
    set("input-cursor", "no")?;
    set("cursor-autohide", "no")?;

    if display == DisplayBackend::Other {
        set("input-vo-cursor", "no")?;
        set("input-keyboard", "no")?;
    }

    // Disable mpv's clipboard so it keeps a single wl_display connection.
    if display == DisplayBackend::Wayland {
        set("clipboard-backends", "")?;
    }

    // Window behavior.
    set("stop-screensaver", "no")?;
    set("keepaspect-window", "no")?;
    set("auto-window-resize", "no")?;
    // Suppress the server-side decoration request on Wayland when the app
    // draws its own client-side decorations; otherwise a compositor titlebar
    // (e.g. KDE) would stack on top of ours.
    let suppress_ssd = display == DisplayBackend::Wayland && client_side_decorations;
    set("border", if suppress_ssd { "no" } else { "yes" })?;
    set("title", "Jellyfin Desktop")?;
    set("wayland-app-id", "org.jellyfin.JellyfinDesktop")?;

    // Keep window open when idle. `force-window=yes` (not "immediate")
    // avoids a macOS deadlock: "immediate" calls handle_force_window
    // inside `mpv_initialize`, which triggers `DispatchQueue.main.sync`
    // while the main thread is blocked in init.
    set("force-window", "yes")?;
    set("idle", "yes")?;

    Ok(())
}

fn apply_boot_options(handle: &Handle, boot: &JfnMpvBoot) -> crate::error::Result<()> {
    let set = |name: &str, value: &str| set_option_or_skip(handle, name, value);
    let set_flag = |name: &str, value: bool| set_option_flag_or_skip(handle, name, value);

    // libmpv defaults config=no (opposite of the mpv CLI); enable it so
    // users' $MPV_HOME/mpv.conf is loaded.
    set("config", "yes")?;
    // We only feed mpv direct media URLs from the Jellyfin server; the
    // youtube-dl/yt-dlp hook would just add startup latency.
    set("ytdl", "no")?;

    if let Some(ua) = unsafe { cstr_opt(boot.user_agent) } {
        set("user-agent", &ua)?;
    }
    if let Some(hwdec) = unsafe { cstr_opt(boot.hwdec) } {
        set("hwdec", &hwdec)?;
    }
    if let Some(geom) = unsafe { cstr_opt(boot.geometry) } {
        set("geometry", &geom)?;
    }
    if boot.force_window_position {
        set("force-window-position", "yes")?;
    }
    if boot.window_maximized_at_boot {
        set("window-maximized", "yes")?;
    }
    if let Some(spdif) = unsafe { cstr_opt(boot.audio_passthrough) }
        && !spdif.is_empty()
    {
        set("audio-spdif", &spdif)?;
    }
    if boot.audio_exclusive {
        set_flag("audio-exclusive", true)?;
    }
    if let Some(ch) = unsafe { cstr_opt(boot.audio_channels) }
        && !ch.is_empty()
    {
        set("audio-channels", &ch)?;
    }
    Ok(())
}

/// Create + configure + initialize the libmpv handle. On success, the
/// raw `mpv_handle*` is returned for callers to borrow. On failure,
/// returns null and any partially-initialized handle is destroyed
/// before returning.
///
/// # Safety
/// `boot` must point to a valid `JfnMpvBoot` whose string fields are
/// either null or NUL-terminated UTF-8 valid for the call.
pub unsafe fn jfn_mpv_handle_init(boot: *const JfnMpvBoot) -> *mut sys::mpv_handle {
    if boot.is_null() {
        return ptr::null_mut();
    }
    let boot = unsafe { &*boot };
    let display = DisplayBackend::from_raw(boot.display_backend);

    let handle = match Handle::create() {
        Ok(h) => h,
        Err(e) => {
            tracing::error!(target: "mpv", "mpv_create failed: {:?}", e);
            return ptr::null_mut();
        }
    };

    if let Err(e) = apply_defaults(&handle, display, boot.client_side_decorations) {
        tracing::error!(target: "mpv", "apply_defaults failed: {:?}", e);
        return ptr::null_mut();
    }
    if let Err(e) = apply_boot_options(&handle, boot) {
        tracing::error!(target: "mpv", "apply_boot_options failed: {:?}", e);
        return ptr::null_mut();
    }

    // Wakeup callback exists only to unstick mpv_wait_event during
    // shutdown.
    handle.set_wakeup_callback(|| {});

    if let Err(e) = handle.initialize() {
        tracing::error!(target: "mpv", "mpv_initialize failed: {:?}", e);
        return ptr::null_mut();
    }

    // mpv log subscription. Token is the same one
    // `mpv_request_log_messages` accepts directly.
    if let Some(level) = unsafe { cstr_opt(boot.mpv_log_level) }
        && !level.is_empty()
    {
        unsafe {
            use std::ffi::CString;
            if let Ok(c) = CString::new(level) {
                sys::mpv_request_log_messages(handle.raw(), c.as_ptr());
            }
        }
    }

    let raw = handle.raw();
    *handle_slot().lock() = Some(handle);
    raw
}

/// Tear down the handle owned by [`jfn_mpv_handle_init`].
/// Idempotent — repeated calls are no-ops.
///
/// On macOS the caller must invoke this off the main thread (mpv's VO
/// uninit does `DispatchQueue.main.sync`).
pub fn jfn_mpv_handle_terminate() {
    let _ = handle_slot().lock().take();
}

/// Borrow the live raw `mpv_handle*`. Returns null before
/// [`jfn_mpv_handle_init`] succeeds and after
/// [`jfn_mpv_handle_terminate`].
pub fn jfn_mpv_handle_get() -> *mut sys::mpv_handle {
    current_raw_handle().unwrap_or(ptr::null_mut())
}

/// Returns the live mpv handle. `None` until [`jfn_mpv_handle_init`]
/// has succeeded.
pub fn current_raw_handle() -> Option<*mut sys::mpv_handle> {
    handle_slot().lock().as_ref().map(|h| h.raw())
}

/// Wake the live handle's `mpv_wait_event` from any thread. No-op if
/// the handle is not currently initialized.
pub fn wakeup_current() {
    if let Some(raw) = current_raw_handle() {
        unsafe { sys::mpv_wakeup(raw) };
    }
}
