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

    tracing::info!(target: "main", "jellyfin-desktop {APP_VERSION_FULL}");
    tracing::info!(target: "main", "CEF {APP_CEF_VERSION}");
    if !log_path.is_empty() {
        tracing::info!(target: "main", "Log file: {log_path}");
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
