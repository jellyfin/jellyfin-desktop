//! End-to-end handle bring-up driven from C++ main(). Replaces the
//! prior C++ `MpvHandle::Create` + `SetDefaults` + per-arg option
//! setters + `Initialize` + `SetLogLevel` sequence with a single
//! `jfn_mpv_handle_init` C entry point.
//!
//! After init the raw `mpv_handle*` is returned for the C++ MpvHandle
//! wrapper to borrow. Rust owns the lifetime: a process-global slot
//! retains the [`Handle`], and `jfn_mpv_handle_terminate` drops it,
//! calling `mpv_terminate_destroy` via [`Handle::Drop`].

use std::ffi::CStr;
use std::os::raw::c_char;
use std::ptr;
use std::sync::Mutex;
use std::sync::OnceLock;

use crate::handle::Handle;
use crate::sys;

/// Display backend the C++ side reports. Matches the discriminants of
/// `enum class DisplayBackend` so the C ABI need not negotiate names.
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

/// Boot-time configuration handed to `jfn_mpv_handle_init`. Mirrors
/// every option the prior C++ path applied between `mpv_create` and
/// `mpv_initialize`. All string fields are NUL-terminated UTF-8 or
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

fn apply_defaults(handle: &Handle, display: DisplayBackend) -> crate::error::Result<()> {
    // Mirror src/mpv/handle.h `SetDefaults`.

    // OSD/OSC off — CEF overlay handles all UI.
    handle.set_option_string("osd-level", "0")?;
    handle.set_option_string("osc", "no")?;
    handle.set_option_string("display-tags", "")?;

    // Track selection is owned by Jellyfin. Disable mpv's heuristic
    // so unspecified tracks stay disabled instead of being auto-picked
    // by language / default-flag / codec scoring.
    handle.set_option_string("track-auto-selection", "no")?;

    // Input: we own all devices and route through CEF.
    handle.set_option_string("input-default-bindings", "no")?;
    handle.set_option_string("input-vo-keyboard", "no")?;
    handle.set_option_string("input-vo-cursor", "no")?;
    handle.set_option_string("input-cursor", "no")?;

    // X11's WM_DELETE_WINDOW routes through mpv's input system as
    // CLOSE_WIN — input-keyboard=no there drops it, breaking the
    // close button. Keep input-keyboard enabled only on X11.
    #[cfg(target_os = "windows")]
    let disable_input_keyboard = true;
    #[cfg(target_os = "macos")]
    let disable_input_keyboard = true;
    #[cfg(all(unix, not(target_os = "macos")))]
    let disable_input_keyboard = display == DisplayBackend::Wayland;
    if disable_input_keyboard {
        handle.set_option_string("input-keyboard", "no")?;
    }
    let _ = display; // referenced under cfg above; silence on other targets

    // Window behavior.
    handle.set_option_string("stop-screensaver", "no")?;
    handle.set_option_string("keepaspect-window", "no")?;
    handle.set_option_string("auto-window-resize", "no")?;
    handle.set_option_string("border", "yes")?;
    handle.set_option_string("title", "Jellyfin Desktop")?;
    handle.set_option_string("wayland-app-id", "org.jellyfin.JellyfinDesktop")?;

    // Keep window open when idle. `force-window=yes` (not "immediate")
    // avoids a macOS deadlock: "immediate" calls handle_force_window
    // inside `mpv_initialize`, which triggers `DispatchQueue.main.sync`
    // while the main thread is blocked in init.
    handle.set_option_string("force-window", "yes")?;
    handle.set_option_string("idle", "yes")?;

    #[cfg(target_os = "macos")]
    unsafe {
        // Used by mpv's macOS Cocoa Common to locate the bundle.
        let key = c"MPVBUNDLE";
        let val = c"true";
        libc::setenv(key.as_ptr(), val.as_ptr(), 1);
    }

    #[cfg(target_os = "windows")]
    {
        // Tell mpv to load the window icon from our exe resources.
        unsafe extern "C" {
            fn _putenv_s(name: *const c_char, value: *const c_char) -> i32;
        }
        let key = c"MPV_WINDOW_ICON";
        let val = c"IDI_ICON1";
        unsafe {
            _putenv_s(key.as_ptr(), val.as_ptr());
        }
    }

    Ok(())
}

fn apply_boot_options(handle: &Handle, boot: &JfnMpvBoot) -> crate::error::Result<()> {
    // libmpv defaults config=no (opposite of the mpv CLI); enable it so
    // users' $MPV_HOME/mpv.conf is loaded.
    handle.set_option_string("config", "yes")?;
    // We only feed mpv direct media URLs from the Jellyfin server; the
    // youtube-dl/yt-dlp hook would just add startup latency.
    handle.set_option_string("ytdl", "no")?;

    if let Some(ua) = unsafe { cstr_opt(boot.user_agent) } {
        handle.set_option_string("user-agent", &ua)?;
    }
    if let Some(hwdec) = unsafe { cstr_opt(boot.hwdec) } {
        handle.set_option_string("hwdec", &hwdec)?;
    }
    if let Some(geom) = unsafe { cstr_opt(boot.geometry) } {
        handle.set_option_string("geometry", &geom)?;
    }
    if boot.force_window_position {
        handle.set_option_string("force-window-position", "yes")?;
    }
    if boot.window_maximized_at_boot {
        handle.set_option_string("window-maximized", "yes")?;
    }
    if let Some(spdif) = unsafe { cstr_opt(boot.audio_passthrough) } {
        if !spdif.is_empty() {
            handle.set_option_string("audio-spdif", &spdif)?;
        }
    }
    if boot.audio_exclusive {
        handle.set_option_flag("audio-exclusive", true)?;
    }
    if let Some(ch) = unsafe { cstr_opt(boot.audio_channels) } {
        if !ch.is_empty() {
            handle.set_option_string("audio-channels", &ch)?;
        }
    }
    Ok(())
}

/// Create + configure + initialize the libmpv handle. On success, the
/// raw `mpv_handle*` is returned for the C++ MpvHandle wrapper to
/// borrow. On failure, returns null and any partially-initialized
/// handle is destroyed before returning.
///
/// # Safety
/// `boot` must point to a valid `JfnMpvBoot` whose string fields are
/// either null or NUL-terminated UTF-8 valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_mpv_handle_init(boot: *const JfnMpvBoot) -> *mut sys::mpv_handle {
    if boot.is_null() {
        return ptr::null_mut();
    }
    let boot = unsafe { &*boot };
    let display = DisplayBackend::from_raw(boot.display_backend);

    let handle = match Handle::create() {
        Ok(h) => h,
        Err(_) => return ptr::null_mut(),
    };

    if apply_defaults(&handle, display).is_err() {
        return ptr::null_mut();
    }
    if apply_boot_options(&handle, boot).is_err() {
        return ptr::null_mut();
    }

    // Wakeup callback exists only to unstick mpv_wait_event during
    // shutdown. No-op closure matches the prior C++ behavior.
    handle.set_wakeup_callback(|| {});

    if handle.initialize().is_err() {
        return ptr::null_mut();
    }

    // mpv log subscription. Token is the same one
    // `mpv_request_log_messages` accepts directly.
    if let Some(level) = unsafe { cstr_opt(boot.mpv_log_level) } {
        if !level.is_empty() {
            unsafe {
                use std::ffi::CString;
                if let Ok(c) = CString::new(level) {
                    sys::mpv_request_log_messages(handle.raw(), c.as_ptr());
                }
            }
        }
    }

    let raw = handle.raw();
    *handle_slot().lock().unwrap() = Some(handle);
    raw
}

/// Tear down the handle owned by [`jfn_mpv_handle_init`].
/// Idempotent — repeated calls are no-ops.
///
/// On macOS the caller must invoke this off the main thread (mpv's VO
/// uninit does `DispatchQueue.main.sync`); see the C++ side's
/// existing teardown thread.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_mpv_handle_terminate() {
    let _ = handle_slot().lock().unwrap().take();
}

/// Borrow the live raw `mpv_handle*`. Returns null before
/// [`jfn_mpv_handle_init`] succeeds and after
/// [`jfn_mpv_handle_terminate`].
#[unsafe(no_mangle)]
pub extern "C" fn jfn_mpv_handle_get() -> *mut sys::mpv_handle {
    current_raw_handle().unwrap_or(ptr::null_mut())
}

/// Rust-side accessor used by sibling crates (e.g. `jfn-playback`) that
/// want to talk to the live handle without round-tripping through the
/// C ABI. Returns `None` until [`jfn_mpv_handle_init`] has succeeded.
pub fn current_raw_handle() -> Option<*mut sys::mpv_handle> {
    handle_slot()
        .lock()
        .unwrap()
        .as_ref()
        .map(|h| h.raw())
}

/// Wake the live handle's `mpv_wait_event` from any thread. No-op if
/// the handle is not currently initialized.
pub fn wakeup_current() {
    if let Some(raw) = current_raw_handle() {
        unsafe { sys::mpv_wakeup(raw) };
    }
}
