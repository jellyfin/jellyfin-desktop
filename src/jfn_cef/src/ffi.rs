//! CEF process bootstrap.

use cef::*;
#[cfg(not(windows))]
use std::ffi::CString;
#[cfg(not(windows))]
use std::os::raw::c_char;
use std::os::raw::c_int;
#[cfg(not(windows))]
use std::sync::OnceLock;

use jfn_platform_abi::DisplayBackend;

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
// (`jfn_cef_set_platform_switches`, `jfn_cef_set_disable_gpu_compositing`,
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
pub fn jfn_cef_start() -> c_int {
    // Platform hook before the FIRST CEF API call (macOS loads the CEF
    // framework here). `try_get`: Linux installs its platform after this
    // runs, and CEF helper subprocesses never install one.
    if let Some(host) = jfn_platform_abi::try_get().and_then(|p| p.cef_host()) {
        host.before_start();
    }
    let _ = api_hash(sys::CEF_API_VERSION_LAST, 0);
    let args = args::Args::new();
    let mut app = JfnAppBuilder::new(JfnApp::new());
    execute_process(
        Some(args.as_main_args()),
        Some(&mut app),
        std::ptr::null_mut(),
    )
}

pub fn jfn_cef_set_log_severity(severity: c_int) {
    state::with_config(|c| c.log_severity = severity);
}

pub fn jfn_cef_set_remote_debugging_port(port: c_int) {
    state::with_config(|c| c.remote_debugging_port = port);
}

pub fn jfn_cef_set_disable_gpu_compositing(disable: bool) {
    if disable {
        state::with_config(|c| {
            c.pending_switches.push(state::PendingSwitch {
                name: "disable-gpu-compositing".to_string(),
                value: None,
            });
        });
    }
}

pub fn jfn_cef_set_platform_switches(backend: DisplayBackend) {
    state::with_config(|c| match backend {
        DisplayBackend::Wayland => {
            c.pending_switches.push(state::PendingSwitch::with_value(
                "ozone-platform",
                "wayland",
            ));
            // OSR honors GetScreenInfo device_scale_factor only without the
            // fractional-scale protocol.
            c.pending_switches.push(state::PendingSwitch::with_value(
                "disable-features",
                "WaylandFractionalScaleV1",
            ));
        }
        DisplayBackend::X11 => {
            c.pending_switches
                .push(state::PendingSwitch::with_value("ozone-platform", "x11"));
        }
        DisplayBackend::MacOS => {
            c.pending_switches
                .push(state::PendingSwitch::flag("single-process"));
            c.pending_switches
                .push(state::PendingSwitch::flag("use-mock-keychain"));
            c.pending_switches
                .push(state::PendingSwitch::with_value("password-store", "basic"));
        }
        DisplayBackend::Windows => {}
    });
}

/// Register the callback invoked from `BrowserProcessHandler::OnContextInitialized`.
/// MUST be called before `jfn_cef_initialize`.
pub fn jfn_cef_set_context_initialized_callback(cb: Option<extern "C" fn()>) {
    state::with_config(|c| c.on_context_initialized = cb);
}

/// Builds CefSettings and calls `CefInitialize`. Returns true on success.
pub fn jfn_cef_initialize() -> bool {
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
        user_agent: CefString::from(concat!(
            "Mozilla/5.0 jellium-desktop/",
            env!("JFN_APP_VERSION")
        )),
        root_cache_path: CefString::from(jfn_paths::cache_dir().to_string_lossy().as_ref()),
        ..Settings::default()
    };
    let cef_host = jfn_platform_abi::try_get().and_then(|p| p.cef_host());
    if cef_host.is_some() {
        settings.external_message_pump = 1;
    } else {
        settings.multi_threaded_message_loop = 1;
    }

    fill_paths(&mut settings);

    // An external pump must install its run-loop hooks before
    // CefInitialize so the first OnScheduleMessagePumpWork (fired
    // synchronously during init) finds them ready.
    if let Some(host) = cef_host {
        host.pump_init();
    }

    // chrome/browser/chrome_browser_main_posix.cc installs SIGINT/SIGTERM
    // handlers during CefInitialize and that path is not gated by
    // disable_signal_handlers. Snapshot the caller's handlers and restore
    // afterward so Chromium's installs are confined to the init window.
    let _sig_guard = jfn_platform_abi::SignalGuard::new();

    let mut app = JfnAppBuilder::new(JfnApp::new());
    let full_argv =
        jfn_platform_abi::try_get().is_some_and(|p| p.display().cef_full_browser_argv());
    let main_args = if full_argv {
        args::Args::new().as_main_args().clone()
    } else {
        // Windows `MainArgs` carries an HINSTANCE, not argv, and always
        // takes the `full_argv` path above — `browser_main_args` is
        // unbuildable there.
        #[cfg(not(windows))]
        {
            browser_main_args()
        }
        #[cfg(windows)]
        {
            args::Args::new().as_main_args().clone()
        }
    };
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
#[cfg(not(windows))]
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
            .unwrap_or_else(|| "jellium-desktop".to_string());
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
    MainArgs {
        argc: c.argc,
        argv: c.argv,
    }
}

pub fn jfn_cef_shutdown() {
    // Gate further external-pump dispatches before tearing down CEF state.
    if let Some(host) = jfn_platform_abi::try_get().and_then(|p| p.cef_host()) {
        host.pump_shutdown();
    }
    shutdown();
}

// ---- helpers ---------------------------------------------------------------

fn log_severity_from_int(v: c_int) -> LogSeverity {
    // cef_log_severity_t is a u32 C enum. Cast through the sys type so we
    // don't depend on private repr details.
    let raw: sys::cef_log_severity_t = unsafe { std::mem::transmute(v as u32) };
    LogSeverity::from(raw)
}

fn fill_paths(settings: &mut Settings) {
    let Some(paths) = jfn_platform_abi::try_get().map(|p| p.cef_paths()) else {
        return;
    };
    let set = |dst: &mut CefString, v: Option<std::path::PathBuf>| {
        if let Some(v) = v {
            *dst = CefString::from(v.to_string_lossy().as_ref());
        }
    };
    set(
        &mut settings.browser_subprocess_path,
        paths.browser_subprocess_path,
    );
    set(&mut settings.framework_dir_path, paths.framework_dir_path);
    set(&mut settings.resources_dir_path, paths.resources_dir_path);
    set(&mut settings.locales_dir_path, paths.locales_dir_path);
}
