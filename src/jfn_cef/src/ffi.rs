//! C ABI surface for CEF process bootstrap.

use cef::*;
#[cfg(target_os = "linux")]
use std::ffi::CStr;
#[cfg(not(target_os = "windows"))]
use std::ffi::CString;
use std::os::raw::{c_char, c_int};
#[cfg(not(target_os = "windows"))]
use std::sync::OnceLock;

use crate::app::{JfnApp, JfnAppBuilder};
use crate::state;

// jfn constructs Chromium's `MainArgs` itself. Two entry points:
//
// * Browser process (`jfn_cef_initialize`): `MainArgs` is `[argv[0]]` —
//   see `browser_main_args`. Chromium's `base::CommandLine` parses only
//   the program name; no jfn CLI flag is ever in there.
// * Subprocess (`jfn_cef_start`): Chromium spawned this binary with an
//   argv it authored itself (`--type=renderer …` etc.). That argv is
//   forwarded to `execute_process` so CEF can dispatch on `--type=`.
//
// Every Chromium switch jfn wants set is pushed by an explicit setter
// (`jfn_cef_set_ozone_platform`, `jfn_cef_set_disable_gpu_compositing`,
// …) onto `state::pending_switches`, then drained into the browser's
// `CefCommandLine` by `on_before_command_line_processing`. The setter
// is the only mapping between jfn's CLI namespace and Chromium's switch
// namespace. Name collisions are coincidence — the setter writes the
// Chromium switch name in code, not by passthrough.
//
// CEF refcounts the App; constructing a fresh one in each FFI call is
// safe because the underlying object outlives the local once CEF has
// captured its reference.

/// Subprocess dispatch + browser-process App construction.
/// Returns -1 in the browser process (continue startup); returns the
/// subprocess exit code otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_cef_start(_argc: c_int, _argv: *const *const c_char) -> c_int {
    // macOS distributes CEF as a framework loaded at runtime via a thunk table
    // in libcef_dll_wrapper (`cef_load_library` populates it). Without this,
    // every CEF call dispatches through a NULL pointer.
    #[cfg(target_os = "macos")]
    {
        static LOADER: OnceLock<cef::library_loader::LibraryLoader> = OnceLock::new();
        LOADER.get_or_init(|| {
            let exe = std::env::current_exe().expect("current_exe");
            let loader = cef::library_loader::LibraryLoader::new(&exe, false);
            assert!(loader.load(), "failed to load Chromium Embedded Framework");
            loader
        });
    }
    let _ = api_hash(sys::CEF_API_VERSION_LAST, 0);
    let args = args::Args::new();
    let mut app = JfnAppBuilder::new(JfnApp::new());
    execute_process(Some(args.as_main_args()), Some(&mut app), std::ptr::null_mut())
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_cef_set_log_severity(severity: c_int) {
    state::with_config(|c| c.log_severity = severity);
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_cef_set_remote_debugging_port(port: c_int) {
    state::with_config(|c| c.remote_debugging_port = port);
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_cef_set_disable_gpu_compositing(disable: bool) {
    if disable {
        state::with_config(|c| {
            c.pending_switches.push(state::PendingSwitch {
                name: "disable-gpu-compositing".to_string(),
                value: None,
            });
        });
    }
}

/// Linux only — Ozone platform selection. `platform_utf8` may be null or
/// empty (no-op). When set to "wayland", also disables the fractional-scale
/// protocol so OSR's GetScreenInfo device_scale_factor is honored.
#[cfg(target_os = "linux")]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_cef_set_ozone_platform(platform_utf8: *const c_char) {
    if platform_utf8.is_null() {
        return;
    }
    let s = unsafe { CStr::from_ptr(platform_utf8) }
        .to_string_lossy()
        .into_owned();
    if s.is_empty() {
        return;
    }
    state::with_config(|c| {
        c.pending_switches.push(state::PendingSwitch {
            name: "ozone-platform".to_string(),
            value: Some(s.clone()),
        });
        if s == "wayland" {
            c.pending_switches.push(state::PendingSwitch {
                name: "disable-features".to_string(),
                value: Some("WaylandFractionalScaleV1".to_string()),
            });
        }
    });
}

/// Register the callback invoked from `BrowserProcessHandler::OnContextInitialized`.
/// MUST be called before `jfn_cef_initialize`.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_cef_set_context_initialized_callback(cb: Option<extern "C" fn()>) {
    state::with_config(|c| c.on_context_initialized = cb);
}

/// Builds CefSettings and calls `CefInitialize`. Returns true on success.
#[unsafe(no_mangle)]
pub extern "C" fn jfn_cef_initialize() -> bool {
    let cfg_severity = state::with_config(|c| c.log_severity);
    let cfg_port = state::with_config(|c| c.remote_debugging_port);

    // Settings.json singleton must be initialized before the renderer
    // process reads it during OnContextCreated.
    let settings_path = jfn_paths::config_dir().join("settings.json");
    jfn_config::settings_init(&settings_path);

    let mut settings = Settings {
        no_sandbox: 1,
        windowless_rendering_enabled: 1,
        disable_signal_handlers: 1,
        log_severity: log_severity_from_int(cfg_severity),
        remote_debugging_port: cfg_port,
        locale: CefString::from("en-US"),
        user_agent: CefString::from(
            concat!("Mozilla/5.0 jellyfin-desktop/", env!("JFN_APP_VERSION")),
        ),
        root_cache_path: CefString::from(jfn_paths::cache_dir().to_string_lossy().as_ref()),
        ..Settings::default()
    };
    #[cfg(target_os = "macos")]
    {
        settings.external_message_pump = 1;
    }
    #[cfg(not(target_os = "macos"))]
    {
        settings.multi_threaded_message_loop = 1;
    }

    fill_paths(&mut settings);

    // macOS external pump must install its CFRunLoopSource + CFRunLoopTimer
    // before CefInitialize so the first OnScheduleMessagePumpWork (fired
    // synchronously during init) finds them ready.
    #[cfg(target_os = "macos")]
    crate::pump::init();

    // chrome/browser/chrome_browser_main_posix.cc installs SIGINT/SIGTERM
    // handlers during CefInitialize and that path is not gated by
    // disable_signal_handlers. Snapshot the caller's handlers and restore
    // afterward so Chromium's installs are confined to the init window.
    let _sig_guard = SignalGuard::new();

    let mut app = JfnAppBuilder::new(JfnApp::new());
    #[cfg(not(target_os = "windows"))]
    let main_args = browser_main_args();
    #[cfg(target_os = "windows")]
    let main_args = args::Args::new().as_main_args().clone();
    initialize(
        Some(&main_args),
        Some(&settings),
        Some(&mut app),
        std::ptr::null_mut(),
    ) == 1
}

// Construct Chromium's browser-process `MainArgs` as `[argv[0]]`. The
// CString + pointer Vec are leaked into process-lifetime statics because
// CEF retains the raw pointers past the `initialize()` call (and across
// the lifetime of the run loop on some code paths).
#[cfg(not(target_os = "windows"))]
fn browser_main_args() -> MainArgs {
    struct CleanArgv {
        argc: c_int,
        argv: *mut *mut c_char,
    }
    // The pointers are valid for the process lifetime (leaked) and we
    // only hand them to CEF, which treats them as immutable input.
    unsafe impl Send for CleanArgv {}
    unsafe impl Sync for CleanArgv {}

    static CLEAN: OnceLock<CleanArgv> = OnceLock::new();
    let c = CLEAN.get_or_init(|| {
        let program = std::env::args()
            .next()
            .unwrap_or_else(|| "jellyfin-desktop".to_string());
        let cstr = CString::new(program).unwrap_or_default();
        let cstr_ptr = cstr.as_ptr() as *mut c_char;
        // Keep the backing buffer alive for the process lifetime.
        Box::leak(Box::new(cstr));
        let argv_vec: Vec<*mut c_char> = vec![cstr_ptr];
        let leaked: &'static mut Vec<*mut c_char> = Box::leak(Box::new(argv_vec));
        CleanArgv {
            argc: leaked.len() as c_int,
            argv: leaked.as_mut_ptr(),
        }
    });
    MainArgs { argc: c.argc, argv: c.argv }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_cef_shutdown() {
    // Gate further external-pump dispatches before tearing down CEF state.
    #[cfg(target_os = "macos")]
    crate::pump::shutdown();
    shutdown();
}

// ---- helpers ---------------------------------------------------------------

fn log_severity_from_int(v: c_int) -> LogSeverity {
    // cef_log_severity_t is a u32 C enum. Cast through the sys type so we
    // don't depend on private repr details.
    let raw: sys::cef_log_severity_t = unsafe { std::mem::transmute(v as u32) };
    LogSeverity::from(raw)
}

#[cfg(target_os = "macos")]
fn fill_paths(settings: &mut Settings) {
    use std::path::PathBuf;
    let mut buf = vec![0u8; 4096];
    let mut size = buf.len() as u32;
    unsafe {
        // _NSGetExecutablePath signature: (char* buf, uint32_t* bufsize) -> i32
        unsafe extern "C" {
            fn _NSGetExecutablePath(buf: *mut c_char, size: *mut u32) -> i32;
        }
        _NSGetExecutablePath(buf.as_mut_ptr() as *mut _, &mut size);
    }
    let nul = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    let exe = std::fs::canonicalize(PathBuf::from(
        std::str::from_utf8(&buf[..nul]).unwrap_or(""),
    ))
    .unwrap_or_default();
    let app_contents = exe.parent().and_then(|p| p.parent()).unwrap_or(&exe);
    let fw = app_contents
        .join("Frameworks")
        .join("Chromium Embedded Framework.framework");
    settings.framework_dir_path = CefString::from(fw.to_string_lossy().as_ref());
    settings.browser_subprocess_path = CefString::from(exe.to_string_lossy().as_ref());
}

#[cfg(target_os = "windows")]
fn fill_paths(settings: &mut Settings) {
    let exe = std::env::current_exe()
        .and_then(std::fs::canonicalize)
        .unwrap_or_default();
    let dir = exe.parent().map(|p| p.to_path_buf()).unwrap_or_default();
    settings.browser_subprocess_path = CefString::from(exe.to_string_lossy().as_ref());
    settings.resources_dir_path = CefString::from(dir.to_string_lossy().as_ref());
    settings.locales_dir_path =
        CefString::from(dir.join("locales").to_string_lossy().as_ref());
}

#[cfg(target_os = "linux")]
fn fill_paths(settings: &mut Settings) {
    let exe = std::fs::canonicalize("/proc/self/exe").unwrap_or_default();
    settings.browser_subprocess_path = CefString::from(exe.to_string_lossy().as_ref());

    let res_dir = option_env!("CEF_RESOURCES_DIR")
        .map(|s| s.to_string())
        .unwrap_or_else(|| {
            exe.parent()
                .map(|p| p.to_string_lossy().into_owned())
                .unwrap_or_default()
        });
    settings.resources_dir_path = CefString::from(res_dir.as_str());
    settings.locales_dir_path = CefString::from(format!("{res_dir}/locales").as_str());
}

#[cfg(not(target_os = "windows"))]
struct SignalGuard {
    int_act: libc::sigaction,
    term_act: libc::sigaction,
}

#[cfg(not(target_os = "windows"))]
impl SignalGuard {
    fn new() -> Self {
        let mut int_act: libc::sigaction = unsafe { std::mem::zeroed() };
        let mut term_act: libc::sigaction = unsafe { std::mem::zeroed() };
        unsafe {
            libc::sigaction(libc::SIGINT, std::ptr::null(), &mut int_act);
            libc::sigaction(libc::SIGTERM, std::ptr::null(), &mut term_act);
        }
        Self { int_act, term_act }
    }
}

#[cfg(not(target_os = "windows"))]
impl Drop for SignalGuard {
    fn drop(&mut self) {
        unsafe {
            libc::sigaction(libc::SIGINT, &self.int_act, std::ptr::null_mut());
            libc::sigaction(libc::SIGTERM, &self.term_act, std::ptr::null_mut());
        }
    }
}

#[cfg(target_os = "windows")]
struct SignalGuard;
#[cfg(target_os = "windows")]
impl SignalGuard {
    fn new() -> Self {
        Self
    }
}
