//! Process entry point. C++ main.cpp shrinks to a forwarder that calls
//! [`jfn_app_main`]; new logic moves out of main.cpp into this module
//! incrementally.
//!
//! `jfn_app_main` returns:
//!   * `>= 0`  exit code — process should terminate (CEF subprocess
//!             return code, `--help` / `--version`, CLI error).
//!   * `-1`    continue in C++ main.cpp (remainder of the port).

use std::ffi::{CStr, CString, c_char, c_int};
use std::ptr;
use std::sync::OnceLock;

use jfn_cef::{APP_CEF_VERSION, APP_VERSION_FULL};

// DisplayBackend discriminants must match `enum class DisplayBackend`
// in `src/platform/display_backend.h`. Pinned by the static_asserts in
// platform_ops.cpp.
#[repr(i32)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
#[allow(dead_code)]
pub enum DisplayBackend {
    Wayland = 0,
    X11 = 1,
    Windows = 2,
    MacOS = 3,
}

// Linux: the `Platform` vtable lives in jfn-wayland / jfn-x11; both crates
// export `make_*_platform` with C linkage returning the same C-layout
// struct. The Rust port reads the vtable through narrow C accessors in
// `src/platform/platform_ops.cpp` (added during the port) so we don't have
// to mirror the entire vtable shape in this crate.
#[cfg(target_os = "linux")]
unsafe extern "C" {
    fn make_wayland_platform() -> jfn_wayland::make_platform::Platform;
    fn make_x11_platform() -> jfn_wayland::make_platform::Platform;
    fn jfn_g_platform_ptr() -> *mut jfn_wayland::make_platform::Platform;
    fn jfn_platform_ops() -> *const jfn_cef::platform_ops::JfnPlatformOps;
}

// Narrow accessors over `g_platform` (defined in platform_ops.cpp). Used
// during the main.cpp -> Rust port so this crate doesn't need a full
// Platform mirror.
unsafe extern "C" {
    fn jfn_g_platform_display() -> c_int;
    fn jfn_g_platform_get_scale() -> f32;
    fn jfn_g_platform_get_display_scale(x: c_int, y: c_int) -> f32;
    fn jfn_g_platform_clamp_window_geometry(
        w: *mut c_int,
        h: *mut c_int,
        x: *mut c_int,
        y: *mut c_int,
    );
    fn jfn_g_platform_pump();
    fn jfn_g_platform_query_window_position(x: *mut c_int, y: *mut c_int) -> bool;
    fn jfn_g_platform_post_window_cleanup();
}

const LOG_MAIN: u8 = 0;
const DEFAULT_LOG_FILTER: &str = "info";

/// Subset of the legacy `cli::Args` consumed by what's still in main.cpp.
/// String fields are CString-owned by Rust; pointers are valid for the
/// lifetime of the process (we hold the storage in [`BOOT_ARGS_STORAGE`]).
#[repr(C)]
pub struct JfnBootArgs {
    pub hwdec: *const c_char,
    pub audio_passthrough: *const c_char,
    pub audio_channels: *const c_char,
    pub log_level: *const c_char,
    pub ozone_platform: *const c_char,
    pub platform_override: *const c_char,
    pub audio_exclusive: bool,
    pub disable_gpu_compositing: bool,
    pub remote_debugging_port: c_int,
}

struct BootArgsStorage {
    _hwdec: CString,
    _audio_passthrough: CString,
    _audio_channels: CString,
    _log_level: CString,
    _ozone_platform: CString,
    _platform_override: CString,
    flat: JfnBootArgs,
}

static BOOT_ARGS_STORAGE: OnceLock<Box<BootArgsStorage>> = OnceLock::new();

unsafe fn take_owned_cstring(p: *mut c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    let s = unsafe { CStr::from_ptr(p) }
        .to_string_lossy()
        .into_owned();
    unsafe { jfn_config::jfn_settings_free_string(p) };
    s
}

unsafe fn take_owned_paths_string(p: *mut c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    let s = unsafe { CStr::from_ptr(p) }
        .to_string_lossy()
        .into_owned();
    unsafe { jfn_paths::jfn_paths_free(p) };
    s
}

unsafe fn cstr_to_string(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
    }
}

fn cs(s: &str) -> CString {
    CString::new(s).unwrap_or_default()
}

/// Normalize the audio-passthrough list: if `dts-hd` is present, drop bare
/// `dts` (the HD variant subsumes it). Mirrors the C++ inline logic in
/// `main.cpp`.
fn normalize_passthrough(s: &str) -> String {
    if !s.contains("dts-hd") {
        return s.to_string();
    }
    s.split(',')
        .filter(|c| *c != "dts")
        .collect::<Vec<_>>()
        .join(",")
}

fn print_help() {
    let hwdec_default = jfn_mpv::HWDEC_DEFAULT;
    println!("Usage: jellyfin-desktop [options]\n");
    println!("Options:");
    println!("  -h, --help                Show this help");
    println!("  -v, --version             Show version");
    println!("  --log-level <filter>      e.g. info | debug | debug,mpv=trace,CEF=off (default: {DEFAULT_LOG_FILTER})");
    println!("  --log-file <path>         Write logs to file ('' to disable)");
    println!("  --hwdec <mode>            Hardware decoding mode (default: {hwdec_default})");
    println!("  --audio-passthrough <codecs>  e.g. ac3,dts-hd,eac3,truehd");
    println!("  --audio-exclusive         Exclusive audio output");
    println!("  --audio-channels <layout> e.g. stereo, 5.1, 7.1");
    println!("  --remote-debug-port <port> Chrome remote debugging");
    println!("  --disable-gpu-compositing Disable CEF GPU compositing");
    println!("  --ozone-platform <plat>   CEF ozone platform (default: follows --platform)");
    if cfg!(target_os = "linux") {
        println!("  --platform <wayland|x11>  Force display backend (Linux only)");
    }
}

fn print_version() {
    println!("jellyfin-desktop {}\n\nCEF {}\n", APP_VERSION_FULL, APP_CEF_VERSION);
    use std::io::Write;
    let _ = std::io::stdout().flush();
    jfn_mpv::probe::jfn_mpv_print_version_info();
}

/// Subprocess dispatch + early-boot (settings/CLI/logging).
///
/// # Safety
/// `argv` must point to `argc` valid NUL-terminated C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_app_main(argc: c_int, argv: *const *const c_char) -> c_int {
    // 1. CEF subprocess dispatch: returns >= 0 in renderer/GPU/utility
    //    subprocesses (subprocess exit code), -1 in the browser process.
    let rc = jfn_cef::ffi::jfn_cef_start(argc, argv);
    if rc >= 0 {
        return rc;
    }

    // 2. Settings init + load.
    let config_dir = unsafe { take_owned_paths_string(jfn_paths::jfn_paths_config_dir()) };
    let settings_path = cs(&format!("{config_dir}/settings.json"));
    unsafe { jfn_config::jfn_settings_init(settings_path.as_ptr()) };
    jfn_config::jfn_settings_load();

    // 3. Seed CLI defaults from saved settings.
    let saved_hwdec = unsafe { take_owned_cstring(jfn_config::jfn_settings_get_hwdec()) };
    let saved_pass = unsafe { take_owned_cstring(jfn_config::jfn_settings_get_audio_passthrough()) };
    let saved_chans = unsafe { take_owned_cstring(jfn_config::jfn_settings_get_audio_channels()) };
    let saved_log_level = unsafe { take_owned_cstring(jfn_config::jfn_settings_get_log_level()) };
    let saved_audio_exclusive = jfn_config::jfn_settings_get_audio_exclusive();

    let mpv_hwdec_default = jfn_mpv::HWDEC_DEFAULT.to_string();

    let mut hwdec = if saved_hwdec.is_empty() { mpv_hwdec_default.clone() } else { saved_hwdec };
    let mut audio_passthrough = saved_pass;
    let mut audio_exclusive = saved_audio_exclusive;
    let mut audio_channels = saved_chans;
    let mut log_level = saved_log_level;

    // 4. Parse argv via jfn_cli.
    let have_x11 = cfg!(target_os = "linux");
    let r = unsafe { jfn_cli::jfn_cli_parse(argc, argv, have_x11) };
    if r.is_null() {
        eprintln!("Error: argv parse failed");
        return 1;
    }
    // Reborrow as a reference so the kind read is safe.
    let rref = unsafe { &*r };
    let kind_rc: Option<c_int> = match rref.kind {
        jfn_cli::JfnCliResultKind::Help => {
            print_help();
            Some(0)
        }
        jfn_cli::JfnCliResultKind::Version => {
            print_version();
            Some(0)
        }
        jfn_cli::JfnCliResultKind::Error => {
            let arg = unsafe { cstr_to_string(rref.unknown_arg) };
            eprintln!("Error: unknown argument '{arg}'");
            Some(1)
        }
        jfn_cli::JfnCliResultKind::Continue => None,
    };

    // Pull parsed values before freeing the result.
    let mut ozone_platform = String::new();
    let mut platform_override = String::new();
    let mut log_file: Option<String> = None;
    let mut disable_gpu_compositing = false;
    let mut remote_debugging_port: c_int = 0;

    if kind_rc.is_none() {
        if !rref.hwdec.is_null() {
            hwdec = unsafe { cstr_to_string(rref.hwdec) };
        }
        if !rref.audio_passthrough.is_null() {
            audio_passthrough = unsafe { cstr_to_string(rref.audio_passthrough) };
        }
        if !rref.audio_channels.is_null() {
            audio_channels = unsafe { cstr_to_string(rref.audio_channels) };
        }
        if !rref.log_level.is_null() {
            log_level = unsafe { cstr_to_string(rref.log_level) };
        }
        if rref.log_file_set {
            log_file = Some(unsafe { cstr_to_string(rref.log_file) });
        }
        if !rref.ozone_platform.is_null() {
            ozone_platform = unsafe { cstr_to_string(rref.ozone_platform) };
        }
        if !rref.platform_override.is_null() {
            platform_override = unsafe { cstr_to_string(rref.platform_override) };
        }
        if rref.audio_exclusive_set {
            audio_exclusive = true;
        }
        if rref.disable_gpu_compositing_set {
            disable_gpu_compositing = true;
        }
        if rref.remote_debugging_port != -1 {
            remote_debugging_port = rref.remote_debugging_port;
        }
    }

    unsafe { jfn_cli::jfn_cli_result_free(r) };

    if let Some(code) = kind_rc {
        return code;
    }

    // 5. Validate hwdec.
    if !jfn_mpv::is_valid_hwdec(&hwdec) {
        hwdec = mpv_hwdec_default;
    }

    // 6. Normalize audio_passthrough (dts-hd subsumes dts).
    if !audio_passthrough.is_empty() {
        audio_passthrough = normalize_passthrough(&audio_passthrough);
    }

    // 7. Resolve log file path. Linux: stderr/journalctl is the norm; only
    //    activate file logging when --log-file was passed explicitly.
    //    macOS/Windows: GUI processes have no user-visible stderr, so
    //    default to a platform log file when --log-file is unset.
    let log_path = match log_file {
        Some(p) => p,
        None => {
            if cfg!(target_os = "linux") {
                String::new()
            } else {
                unsafe { take_owned_paths_string(jfn_paths::jfn_paths_log_path()) }
            }
        }
    };

    let filter = if log_level.is_empty() {
        DEFAULT_LOG_FILTER.to_string()
    } else {
        log_level.clone()
    };
    let log_path_c = cs(&log_path);
    let filter_c = cs(&filter);
    unsafe { jfn_logging::jfn_log_init(log_path_c.as_ptr(), filter_c.as_ptr()) };

    tracing::info!(target: "Main", "jellyfin-desktop {APP_VERSION_FULL}");
    tracing::info!(target: "Main", "CEF {APP_CEF_VERSION}");
    if !log_path.is_empty() {
        tracing::info!(target: "Main", "Log file: {log_path}");
    }

    // 8. Stash parsed args so the remaining C++ body can read them while
    //    the port is in progress. Heap-allocated; the Box never moves so
    //    the CString::as_ptr() values stored in `flat` stay valid.
    let mut storage = Box::new(BootArgsStorage {
        _hwdec: cs(&hwdec),
        _audio_passthrough: cs(&audio_passthrough),
        _audio_channels: cs(&audio_channels),
        _log_level: cs(&log_level),
        _ozone_platform: cs(&ozone_platform),
        _platform_override: cs(&platform_override),
        flat: JfnBootArgs {
            hwdec: ptr::null(),
            audio_passthrough: ptr::null(),
            audio_channels: ptr::null(),
            log_level: ptr::null(),
            ozone_platform: ptr::null(),
            platform_override: ptr::null(),
            audio_exclusive,
            disable_gpu_compositing,
            remote_debugging_port,
        },
    });
    // 9. Linux: pick display backend, populate g_platform, run early_init,
    //    register the platform-ops vtable with the Rust-side jfn-cef.
    //    Windows/macOS: g_platform was populated by main() before jfn_app_main
    //    returned (we ran before CefExecuteProcess on those platforms).
    #[cfg(target_os = "linux")]
    {
        let backend = if platform_override == "wayland" {
            DisplayBackend::Wayland
        } else if platform_override == "x11" {
            DisplayBackend::X11
        } else if !platform_override.is_empty() {
            eprintln!(
                "Unknown platform: {} (expected wayland or x11)",
                platform_override
            );
            return 1;
        } else {
            let has_wayland = std::env::var_os("WAYLAND_DISPLAY").is_some();
            let has_display = std::env::var_os("DISPLAY").is_some();
            if has_wayland || !has_display {
                DisplayBackend::Wayland
            } else {
                DisplayBackend::X11
            }
        };
        unsafe {
            let p = match backend {
                DisplayBackend::Wayland => make_wayland_platform(),
                DisplayBackend::X11 => make_x11_platform(),
                _ => unreachable!(),
            };
            *jfn_g_platform_ptr() = p;
            // Mirror `make_platform()`'s C++ inline: early_init then wire
            // platform_ops into jfn-cef.
            if let Some(ei) = (*jfn_g_platform_ptr()).early_init {
                ei();
            }
            jfn_cef::platform_ops::jfn_cef_set_platform_ops(jfn_platform_ops());
        }
        tracing::info!(target: "Main", "Display backend: {}",
            if backend == DisplayBackend::Wayland { "wayland" } else { "x11" });
    }

    // 10. Install signal handler (Unix) / Windows ConsoleCtrl handler.
    install_signal_handler();

    // 11. Single-instance check (Linux + Windows; macOS uses NSApp delegate
    //     activation).
    #[cfg(not(target_os = "macos"))]
    {
        if jfn_single_instance::jfn_single_instance_try_signal_existing() != 0 {
            tracing::info!(target: "Main", "Signaled existing instance, exiting");
            return 0;
        }
        unsafe extern "C" fn on_activate(_token: *const c_char, _userdata: *mut std::ffi::c_void) {
            // TODO: raise window via xdg-activation
        }
        let ok = unsafe {
            jfn_single_instance::jfn_single_instance_start_listener(
                Some(on_activate),
                ptr::null_mut(),
            )
        };
        if ok == 0 {
            tracing::warn!(target: "Main", "Single-instance listener failed to start");
        }
        // Stop on process exit. Held in a Drop guard via static slot below.
        install_listener_guard();
    }

    // 12. Export MPV_HOME so libmpv reads our packaged config dir.
    {
        let mpv_home = unsafe { take_owned_paths_string(jfn_paths::jfn_paths_mpv_home()) };
        #[cfg(unix)]
        unsafe {
            std::env::set_var("MPV_HOME", &mpv_home);
        }
        #[cfg(windows)]
        unsafe {
            std::env::set_var("MPV_HOME", &mpv_home);
        }
        let _ = mpv_home;
    }

    // 13. Linux/Wayland: start the wl-proxy that intercepts xdg_toplevel
    //     configure + fractional-scale events for the mpv subwindow.
    #[cfg(target_os = "linux")]
    {
        // Read backend as i32 discriminant; matches the C++ enum class.
        let backend_now: i32 = unsafe {
            *(jfn_g_platform_ptr() as *const i32)
        };
        if backend_now == DisplayBackend::Wayland as i32 {
            unsafe { start_wlproxy() };
        }
    }

    // 14. Compute boot geometry from saved window geometry. mpv's
    //     --geometry takes physical pixels (see m_geometry_apply in
    //     third_party/mpv/options/m_option.c). The post-CEF resize block
    //     in run_with_cef corrects scale drift once display-hidpi-scale
    //     is known.
    let (boot_geometry, boot_force_position, boot_window_max) = compute_boot_geometry();

    // 15. Pick libmpv log subscription level matching what jfn-logging
    //     would actually surface for LOG_MPV. mpv's "v" maps to Debug;
    //     "debug" maps to Trace. Cap at "debug".
    let mpv_log_level = mpv_log_level_from_filter();

    // 16. Initialise the mpv handle via the Rust boot path.
    let backend_byte: u8 = unsafe { jfn_g_platform_display() as u8 };
    let geometry_c = cs(&boot_geometry);
    let hwdec_c = cs(&hwdec);
    let user_agent_c = cs(&format!("JellyfinDesktop/{}", APP_VERSION_FULL));
    let passthrough_c = cs(&audio_passthrough);
    let channels_c = cs(&audio_channels);
    let mpv_log_level_c = cs(mpv_log_level);
    let boot = jfn_mpv::boot::JfnMpvBoot {
        display_backend: backend_byte,
        hwdec: hwdec_c.as_ptr(),
        user_agent: user_agent_c.as_ptr(),
        audio_passthrough: if audio_passthrough.is_empty() { ptr::null() } else { passthrough_c.as_ptr() },
        audio_exclusive,
        audio_channels: if audio_channels.is_empty() { ptr::null() } else { channels_c.as_ptr() },
        geometry: geometry_c.as_ptr(),
        force_window_position: boot_force_position,
        window_maximized_at_boot: boot_window_max,
        mpv_log_level: mpv_log_level_c.as_ptr(),
    };
    let raw = unsafe { jfn_mpv::boot::jfn_mpv_handle_init(&boot as *const _) };
    if raw.is_null() {
        tracing::error!(target: "Main", "mpv handle init failed");
        return 1;
    }

    // 17. Register Rust ingest-layer property observations.
    if !jfn_playback::ingest_driver::jfn_playback_observe_mpv_properties(backend_byte) {
        tracing::error!(target: "Main", "observe_mpv_properties failed");
        return 1;
    }

    // 18. Capture user's mpv.conf bg, force startup color.
    //     force-window=yes (not "immediate") defers VO creation so the
    //     user's color never flashes before the override.
    let user_bg = jfn_mpv::api::jfn_mpv_get_background_color();
    publish_video_bg(user_bg);
    {
        let hex = format!("#{:06x}", user_bg);
        tracing::info!(target: "Main", "video bg captured: {hex}");
    }
    let startup_bg = cs("#101010");
    unsafe { jfn_mpv::api::jfn_mpv_set_background_color_hex(startup_bg.as_ptr()) };

    // 19. Log mpv-version + ffmpeg-version.
    for prop in ["mpv-version", "ffmpeg-version"] {
        let pc = cs(prop);
        let v = unsafe { jfn_mpv::api::jfn_mpv_get_property_string(pc.as_ptr()) };
        let s = if v.is_null() {
            String::new()
        } else {
            let s = unsafe { CStr::from_ptr(v) }.to_string_lossy().into_owned();
            unsafe { jfn_mpv::api::jfn_mpv_free_string(v) };
            s
        };
        tracing::info!(target: "Main", "{prop} {s}");
    }

    // 20. Re-bind CLOSE_WIN -> quit. input-default-bindings=no removes
    //     all builtin bindings including this one; the WM close button
    //     needs it back.
    {
        let kb = cs("keybind");
        let name = cs("CLOSE_WIN");
        let action = cs("quit");
        let argv = [kb.as_ptr(), name.as_ptr(), action.as_ptr(), ptr::null()];
        unsafe { jfn_mpv::sys::mpv_command(raw, argv.as_ptr() as *mut *const c_char) };
    }

    // 21. Wait for the VO window. Drains mpv events into the ingest
    //     layer; stops once OSD pixels are non-zero, the maximize gate
    //     (if requested) flipped, and the Wayland scale is known.
    let want_max = {
        let mut g = jfn_config::JfnWindowGeometry::default();
        unsafe { jfn_config::jfn_settings_get_window_geometry(&mut g) };
        g.maximized
    };
    let wait_for_scale = cfg!(target_os = "linux")
        && unsafe { jfn_g_platform_display() } == DisplayBackend::Wayland as i32;
    let wait_timeout = if wait_for_scale { 0.1 } else { 1.0 };
    tracing::info!(target: "Main", "Waiting for mpv window...");

    let mut mw: i32 = 0;
    let mut mh: i32 = 0;
    let mut need_max = want_max;
    loop {
        #[cfg(target_os = "macos")]
        {
            unsafe { jfn_g_platform_pump() };
            let ev = jfn_mpv::api::jfn_mpv_wait_event(0.0);
            if ev.is_null() { continue; }
            let event_id = unsafe { (*ev).event_id }.0;
            if event_id == 0 { unsafe { libc::usleep(10000) }; continue; }
            if event_id == 2 {
                log_mpv_event(ev);
                continue;
            }
            if event_id == 1 || event_id == 7 { return 0; }
            if consume_vo_event(ev, &mut mw, &mut mh, &mut need_max, wait_for_scale) {
                break;
            }
        }
        #[cfg(not(target_os = "macos"))]
        {
            let ev = jfn_mpv::api::jfn_mpv_wait_event(wait_timeout);
            if ev.is_null() { continue; }
            let event_id = unsafe { (*ev).event_id }.0;
            if event_id == 2 { log_mpv_event(ev); continue; }
            if event_id == 1 || event_id == 7 { return 0; }
            if consume_vo_event(ev, &mut mw, &mut mh, &mut need_max, wait_for_scale) {
                break;
            }
        }
    }
    store_vo_size(mw, mh);

    storage.flat.hwdec = storage._hwdec.as_ptr();
    storage.flat.audio_passthrough = storage._audio_passthrough.as_ptr();
    storage.flat.audio_channels = storage._audio_channels.as_ptr();
    storage.flat.log_level = storage._log_level.as_ptr();
    storage.flat.ozone_platform = storage._ozone_platform.as_ptr();
    storage.flat.platform_override = storage._platform_override.as_ptr();
    let _ = BOOT_ARGS_STORAGE.set(storage);
    let _ = LOG_MAIN;

    -1
}

/// Return a borrowed pointer to the parsed CLI/settings args. Valid for
/// the rest of the process lifetime once [`jfn_app_main`] returns -1.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_app_boot_args() -> *const JfnBootArgs {
    match BOOT_ARGS_STORAGE.get() {
        Some(s) => &s.flat as *const JfnBootArgs,
        None => ptr::null(),
    }
}

// JfnBootArgs holds raw pointers but they all reference the same Box,
// which never moves and is read-only after init — safe to share across
// threads. The Box itself is Send + Sync because of this.
unsafe impl Send for BootArgsStorage {}
unsafe impl Sync for BootArgsStorage {}

// =====================================================================
// Signal handler + listener guard + wlproxy lifetime
// =====================================================================

#[cfg(unix)]
static SIGNAL_GUARD: OnceLock<SignalGuardSlot> = OnceLock::new();

#[cfg(unix)]
struct SignalGuardSlot(*mut jfn_signal_guard::SignalGuard);
#[cfg(unix)]
unsafe impl Send for SignalGuardSlot {}
#[cfg(unix)]
unsafe impl Sync for SignalGuardSlot {}

#[cfg(unix)]
unsafe extern "C" fn on_shutdown_signal(_sig: c_int) {
    jfn_playback::jfn_shutdown_initiate();
}

#[cfg(windows)]
unsafe extern "system" fn console_ctrl_handler(_t: u32) -> i32 {
    jfn_playback::jfn_shutdown_initiate();
    1
}

fn install_signal_handler() {
    #[cfg(unix)]
    {
        let g = unsafe { jfn_signal_guard::jfn_signal_guard_install(Some(on_shutdown_signal)) };
        let _ = SIGNAL_GUARD.set(SignalGuardSlot(g));
    }
    #[cfg(windows)]
    {
        unsafe extern "system" {
            fn SetConsoleCtrlHandler(handler: unsafe extern "system" fn(u32) -> i32, add: i32) -> i32;
        }
        unsafe { SetConsoleCtrlHandler(console_ctrl_handler, 1) };
    }
}

#[cfg(not(target_os = "macos"))]
static LISTENER_GUARD: OnceLock<ListenerGuardSlot> = OnceLock::new();

#[cfg(not(target_os = "macos"))]
struct ListenerGuardSlot;
#[cfg(not(target_os = "macos"))]
impl Drop for ListenerGuardSlot {
    fn drop(&mut self) {
        jfn_single_instance::jfn_single_instance_stop_listener();
    }
}
#[cfg(not(target_os = "macos"))]
unsafe impl Send for ListenerGuardSlot {}
#[cfg(not(target_os = "macos"))]
unsafe impl Sync for ListenerGuardSlot {}

#[cfg(not(target_os = "macos"))]
fn install_listener_guard() {
    let _ = LISTENER_GUARD.set(ListenerGuardSlot);
}

#[cfg(target_os = "linux")]
unsafe extern "C" {
    fn jfn_wlproxy_start() -> *mut std::ffi::c_void;
    fn jfn_wlproxy_display_name(p: *const std::ffi::c_void) -> *const c_char;
    fn jfn_wlproxy_stop(p: *mut std::ffi::c_void);
    fn jfn_wl_register_proxy_callbacks();
}

#[cfg(target_os = "linux")]
static WLPROXY: OnceLock<WlproxySlot> = OnceLock::new();

#[cfg(target_os = "linux")]
struct WlproxySlot(*mut std::ffi::c_void);
#[cfg(target_os = "linux")]
unsafe impl Send for WlproxySlot {}
#[cfg(target_os = "linux")]
unsafe impl Sync for WlproxySlot {}

#[cfg(target_os = "linux")]
unsafe fn start_wlproxy() {
    let p = unsafe { jfn_wlproxy_start() };
    if p.is_null() {
        tracing::error!(target: "Main", "wlproxy start failed; continuing without proxy");
        return;
    }
    let disp_p = unsafe { jfn_wlproxy_display_name(p) };
    if disp_p.is_null() {
        tracing::error!(target: "Main", "wlproxy display name empty; aborting proxy");
        unsafe { jfn_wlproxy_stop(p) };
        return;
    }
    let disp = unsafe { CStr::from_ptr(disp_p) }.to_string_lossy().into_owned();
    if disp.is_empty() {
        tracing::error!(target: "Main", "wlproxy display name empty; aborting proxy");
        unsafe { jfn_wlproxy_stop(p) };
        return;
    }
    tracing::info!(target: "Main", "wlproxy listening on {disp}");
    unsafe { std::env::set_var("WAYLAND_DISPLAY", &disp) };
    // Register the configure intercept BEFORE mpv_create so the first
    // compositor configure (which arrives shortly after mpv_initialize) is
    // captured.
    unsafe { jfn_wl_register_proxy_callbacks() };
    let _ = WLPROXY.set(WlproxySlot(p));
}

/// C accessor for whether jfn_app_main already set up signal/single-instance/
/// platform/wlproxy. Lets main.cpp skip the duplicated work during the port.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_app_boot_done() -> bool {
    BOOT_ARGS_STORAGE.get().is_some()
}

// =====================================================================
// mpv boot helpers + VO wait loop
// =====================================================================

const DEFAULT_LOGICAL_WIDTH: i32 = 1600;
const DEFAULT_LOGICAL_HEIGHT: i32 = 900;

fn compute_boot_geometry() -> (String, bool, bool) {
    let mut g = jfn_config::JfnWindowGeometry::default();
    unsafe { jfn_config::jfn_settings_get_window_geometry(&mut g) };
    let mut x = g.x;
    let mut y = g.y;
    let scale = unsafe { jfn_g_platform_get_display_scale(x, y) };
    let scale_f = if scale > 0.0 { scale } else { 1.0 };
    let (mut w, mut h) = if g.logical_width > 0 && g.logical_height > 0 {
        (
            (g.logical_width as f32 * scale_f).round() as i32,
            (g.logical_height as f32 * scale_f).round() as i32,
        )
    } else if g.width > 0 && g.height > 0 {
        (g.width, g.height)
    } else {
        (
            (DEFAULT_LOGICAL_WIDTH as f32 * scale_f).round() as i32,
            (DEFAULT_LOGICAL_HEIGHT as f32 * scale_f).round() as i32,
        )
    };
    tracing::debug!(target: "Main", "initial scale: {scale_f} -> {w}x{h}");
    unsafe { jfn_g_platform_clamp_window_geometry(&mut w, &mut h, &mut x, &mut y) };
    let mut s = format!("{w}x{h}");
    let force_position = x >= 0 && y >= 0;
    if force_position {
        s.push_str(&format!("+{x}+{y}"));
    }
    (s, force_position, g.maximized)
}

const LOG_MPV: u8 = 1;
const LEVEL_TRACE: u8 = 0;
const LEVEL_DEBUG: u8 = 1;
const LEVEL_INFO: u8 = 2;
const LEVEL_WARN: u8 = 3;
const LEVEL_ERROR: u8 = 4;

fn mpv_log_level_from_filter() -> &'static str {
    let e = jfn_logging::jfn_log_enabled;
    if e(LOG_MPV, LEVEL_TRACE) {
        "debug"
    } else if e(LOG_MPV, LEVEL_DEBUG) {
        "v"
    } else if e(LOG_MPV, LEVEL_INFO) {
        "info"
    } else if e(LOG_MPV, LEVEL_WARN) {
        "warn"
    } else if e(LOG_MPV, LEVEL_ERROR) {
        "error"
    } else {
        "no"
    }
}

fn publish_video_bg(rgb: u32) {
    // Mirror the C++ `g_video_bg = Color{rgb}` write. Color is POD on the
    // C++ side; store via the accessor so platform_ops.cpp keeps owning
    // the global.
    unsafe extern "C" {
        fn jfn_g_video_bg_set(rgb: u32);
    }
    unsafe { jfn_g_video_bg_set(rgb) };
}

fn log_mpv_event(ev: *mut jfn_mpv::sys::mpv_event) {
    let msg = unsafe { (*ev).data as *mut jfn_mpv::sys::mpv_event_log_message };
    if msg.is_null() {
        return;
    }
    let prefix = unsafe { CStr::from_ptr((*msg).prefix) }.to_string_lossy();
    let text = unsafe { CStr::from_ptr((*msg).text) }.to_string_lossy();
    let level = unsafe { (*msg).log_level }.0 as i32;
    // Mirror C++ log_mpv_message: LEVEL_FATAL=10, ERROR=20, WARN=30,
    // INFO=40, V=50, DEBUG=60, TRACE=70.
    match level {
        10 | 20 => tracing::error!(target: "mpv", "{prefix}: {text}"),
        30 => tracing::warn!(target: "mpv", "{prefix}: {text}"),
        40 => tracing::info!(target: "mpv", "{prefix}: {text}"),
        50 => tracing::debug!(target: "mpv", "{prefix}: {text}"),
        60 => tracing::trace!(target: "mpv", "{prefix}: {text}"),
        _ => tracing::warn!(target: "mpv", "[unhandled mpv level {level}] {prefix}: {text}"),
    }
}

const JFN_OBSERVE_WINDOW_MAX: u64 = 11;

fn consume_vo_event(
    ev: *mut jfn_mpv::sys::mpv_event,
    mw: &mut i32,
    mh: &mut i32,
    need_max: &mut bool,
    wait_for_scale: bool,
) -> bool {
    let event_id = unsafe { (*ev).event_id }.0;
    if event_id == 22 {
        // MPV_EVENT_PROPERTY_CHANGE
        let scale_raw = unsafe { jfn_g_platform_get_scale() };
        let scale = if scale_raw > 0.0 { scale_raw } else { 1.0 };
        let has_macos_logical;
        let mut mac_lw: c_int = 0;
        let mut mac_lh: c_int = 0;
        #[cfg(target_os = "macos")]
        unsafe {
            unsafe extern "C" {
                fn jfn_macos_query_logical_content_size(lw: *mut c_int, lh: *mut c_int) -> bool;
            }
            has_macos_logical = jfn_macos_query_logical_content_size(&mut mac_lw, &mut mac_lh);
        }
        #[cfg(not(target_os = "macos"))]
        {
            has_macos_logical = false;
            let _ = (&mut mac_lw, &mut mac_lh);
        }
        unsafe {
            jfn_playback::ingest_driver::jfn_playback_ingest_mpv_event(
                ev as *const _,
                scale,
                has_macos_logical,
                mac_lw,
                mac_lh,
            );
        }
        let reply = unsafe { (*ev).reply_userdata };
        if reply == JFN_OBSERVE_WINDOW_MAX && jfn_playback::ingest_driver::jfn_playback_window_maximized() {
            *need_max = false;
        }
    }
    let pw = jfn_playback::ingest_driver::jfn_playback_osd_pw();
    let ph = jfn_playback::ingest_driver::jfn_playback_osd_ph();
    if pw > 0 && ph > 0 {
        *mw = pw;
        *mh = ph;
    }
    #[cfg(target_os = "linux")]
    let scale_ready = !wait_for_scale || unsafe {
        unsafe extern "C" {
            fn jfn_wl_scale_known() -> bool;
        }
        jfn_wl_scale_known()
    };
    #[cfg(not(target_os = "linux"))]
    let scale_ready = {
        let _ = wait_for_scale;
        true
    };
    *mw > 0 && !*need_max && scale_ready
}

static VO_SIZE: OnceLock<(i32, i32)> = OnceLock::new();

fn store_vo_size(w: i32, h: i32) {
    let _ = VO_SIZE.set((w, h));
}

/// C accessor for the post-wait VO surface size.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_app_vo_size(w: *mut c_int, h: *mut c_int) {
    if let Some((ww, hh)) = VO_SIZE.get() {
        if !w.is_null() {
            unsafe { *w = *ww };
        }
        if !h.is_null() {
            unsafe { *h = *hh };
        }
    }
}

/// Tear down boot-owned resources at process exit (wlproxy, single-instance
/// listener). Called from main.cpp's tail until that path is ported too.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_app_teardown() {
    #[cfg(target_os = "linux")]
    {
        if let Some(slot) = WLPROXY.get() {
            unsafe { jfn_wlproxy_stop(slot.0) };
        }
    }
    // Single-instance listener is dropped via the OnceLock at process exit;
    // no explicit teardown call needed here. SignalGuard slot stays until
    // exit and restores the original disposition via libsignal_guard's Drop.
}
