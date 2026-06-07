//! Process entry point. [`jfn_app_main`] owns the full main loop and
//! returns the exit code.

use std::ffi::{CStr, CString, c_char, c_int};
use std::ptr;
use std::sync::OnceLock;

use clap::Parser;
use jfn_cef::{APP_CEF_VERSION, APP_VERSION_FULL};
use jfn_platform_abi::{DisplayBackend, IdleInhibitLevel, Platform, WindowGeometry};

use crate::cli;

// Shorthand for the installed Platform backend. `install()` happens before
// any of the call sites here run.
fn plat() -> &'static dyn Platform {
    jfn_platform_abi::get()
}

// Read once by `jfn_app_main` after CEF boot to seed the theme rotator.
static VIDEO_BG: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

fn video_bg_set(rgb: u32) {
    VIDEO_BG.store(rgb, std::sync::atomic::Ordering::Release);
}

fn video_bg_get() -> u32 {
    VIDEO_BG.load(std::sync::atomic::Ordering::Acquire)
}

pub(crate) const DEFAULT_LOG_FILTER: &str = "info";

struct BootArgs {
    ozone_platform: String,
    disable_gpu_compositing: bool,
    remote_debugging_port: c_int,
}

fn cs(s: &str) -> CString {
    CString::new(s).unwrap_or_default()
}

/// Normalize the audio-passthrough list: if `dts-hd` is present, drop
/// bare `dts` (the HD variant subsumes it).
fn normalize_passthrough(s: &str) -> String {
    if !s.contains("dts-hd") {
        return s.to_string();
    }
    s.split(',')
        .filter(|c| *c != "dts")
        .collect::<Vec<_>>()
        .join(",")
}

fn print_version() {
    println!(
        "jellyfin-desktop {}\n\nCEF {}\n",
        APP_VERSION_FULL, APP_CEF_VERSION
    );
    use std::io::Write;
    let _ = std::io::stdout().flush();
    jfn_mpv::probe::jfn_mpv_print_version_info();
}

fn init_logging(log_file: Option<String>, log_level: &str) {
    // Linux: stderr/journalctl is the norm; only activate file logging when
    // --log-file was passed explicitly. macOS/Windows: GUI processes have no
    // user-visible stderr, so default to a platform log file when unset.
    let log_path = log_file.unwrap_or_else(|| {
        if cfg!(target_os = "linux") {
            String::new()
        } else {
            jfn_paths::log_path().to_string_lossy().into_owned()
        }
    });

    let filter = if log_level.is_empty() {
        DEFAULT_LOG_FILTER.to_string()
    } else {
        log_level.to_string()
    };
    jfn_logging::jfn_log_init(&log_path, &filter);

    tracing::info!(target: "Main", "jellyfin-desktop {APP_VERSION_FULL}");
    tracing::info!(target: "Main", "CEF {APP_CEF_VERSION}");
    if !log_path.is_empty() {
        tracing::info!(target: "Main", "Log file: {log_path}");
    }
}

fn init_single_instance() -> bool {
    if crate::single_instance::try_signal_existing() {
        tracing::info!(target: "Main", "Signaled existing instance, exiting");
        return false;
    }
    let ok = crate::single_instance::start_listener(|_token: &str| {
        // TODO: raise window via xdg-activation
    });
    if !ok {
        tracing::warn!(target: "Main", "Single-instance listener failed to start");
    }
    // Stop on process exit. Held in a Drop guard via static slot below.
    install_listener_guard();
    true
}

#[cfg(target_os = "linux")]
fn install_linux_platform(platform: Option<cli::PlatformArg>, platform_paint: Option<cli::Paint>) {
    let backend = match platform {
        Some(cli::PlatformArg::Wayland) => DisplayBackend::Wayland,
        Some(cli::PlatformArg::X11) => DisplayBackend::X11,
        None => {
            let has_wayland = std::env::var_os("WAYLAND_DISPLAY").is_some();
            let has_display = std::env::var_os("DISPLAY").is_some();
            if has_wayland || !has_display {
                DisplayBackend::Wayland
            } else {
                DisplayBackend::X11
            }
        }
    };
    if let Some(p) = platform_paint {
        match backend {
            DisplayBackend::Wayland => jfn_wayland::set_paint_override(match p {
                cli::Paint::Dmabuf => jfn_wayland::WlPaintOverride::Dmabuf,
                cli::Paint::Gpu => jfn_wayland::WlPaintOverride::Gpu,
                cli::Paint::Shm => jfn_wayland::WlPaintOverride::Shm,
            }),
            DisplayBackend::X11 => jfn_x11::set_paint_override(match p {
                cli::Paint::Dmabuf => jfn_x11::X11PaintOverride::Dmabuf,
                cli::Paint::Gpu => jfn_x11::X11PaintOverride::Gpu,
                cli::Paint::Shm => jfn_x11::X11PaintOverride::Shm,
            }),
            _ => {}
        }
    }

    let p: Box<dyn Platform> = match backend {
        DisplayBackend::Wayland => jfn_wayland::make_platform::make_wayland_platform(),
        DisplayBackend::X11 => jfn_x11::make_platform::make_x11_platform(),
        _ => unreachable!(),
    };
    p.early_init();
    jfn_platform_abi::install(p);
    tracing::info!(target: "Main", "Display backend: {}",
        if backend == DisplayBackend::Wayland { "wayland" } else { "x11" });
}

fn log_mpv_versions() {
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
}

fn terminate_mpv_handle() {
    // macOS needs to run TerminateDestroy off the main thread (mpv's VO uninit
    // does DispatchQueue.main.sync), so we spawn a side thread and pump CFRunLoop here.
    #[cfg(target_os = "macos")]
    unsafe {
        unsafe extern "C" {
            fn signal(signum: c_int, handler: unsafe extern "C" fn(c_int)) -> usize;
            fn CFRunLoopWakeUp(rl: *const std::ffi::c_void);
            fn CFRunLoopGetMain() -> *const std::ffi::c_void;
            fn CFRunLoopRunInMode(
                mode: *const std::ffi::c_void,
                seconds: f64,
                returnAfterSourceHandled: i32,
            ) -> i32;
            static kCFRunLoopDefaultMode: *const std::ffi::c_void;
        }
        unsafe extern "C" fn sigalrm_noop(_: c_int) {}
        let done = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let d2 = done.clone();
        let t = std::thread::spawn(move || {
            signal(libc::SIGALRM as c_int, sigalrm_noop);
            jfn_mpv::boot::jfn_mpv_handle_terminate();
            d2.store(true, std::sync::atomic::Ordering::Release);
            CFRunLoopWakeUp(CFRunLoopGetMain());
        });
        while !done.load(std::sync::atomic::Ordering::Acquire) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, f64::MAX, 1);
        }
        let _ = t.join();
    }
    #[cfg(not(target_os = "macos"))]
    jfn_mpv::boot::jfn_mpv_handle_terminate();
}

fn install_mpv_close_binding(raw: *mut jfn_mpv::sys::mpv_handle) {
    let kb = cs("keybind");
    let name = cs("CLOSE_WIN");
    let action = cs("quit");
    let argv = [kb.as_ptr(), name.as_ptr(), action.as_ptr(), ptr::null()];
    unsafe { jfn_mpv::sys::mpv_command(raw, argv.as_ptr() as *mut *const c_char) };
}

fn setup_mpv_environment() {
    // Export MPV_HOME so libmpv reads our packaged config dir.
    let mpv_home = jfn_paths::mpv_home();
    unsafe {
        std::env::set_var("MPV_HOME", &mpv_home);
    }
    let _ = mpv_home;

    // Linux/Wayland: start the wl-proxy that intercepts xdg_toplevel
    // configure + fractional-scale events for the mpv subwindow.
    #[cfg(target_os = "linux")]
    {
        if plat().display() == DisplayBackend::Wayland {
            unsafe { start_wlproxy() };
        }
    }
}

struct StartupOptions {
    hwdec: String,
    audio_passthrough: String,
    audio_exclusive: bool,
    audio_channels: String,
    log_level: String,
    ozone_platform: String,
    log_file: Option<String>,
    disable_gpu_compositing: bool,
    remote_debugging_port: c_int,
    #[cfg(target_os = "linux")]
    platform: Option<cli::PlatformArg>,
    #[cfg(target_os = "linux")]
    platform_paint: Option<cli::Paint>,
}

fn resolve_startup_options(cli: cli::Cli) -> StartupOptions {
    // Seed CLI defaults from saved settings.
    let saved_hwdec = jfn_config::hwdec();
    let saved_pass = jfn_config::audio_passthrough();
    let saved_chans = jfn_config::audio_channels();
    let saved_log_level = jfn_config::log_level();
    let saved_audio_exclusive = jfn_config::audio_exclusive();

    let mpv_hwdec_default = jfn_mpv::HWDEC_DEFAULT.to_string();

    let mut hwdec = if saved_hwdec.is_empty() {
        mpv_hwdec_default.clone()
    } else {
        saved_hwdec
    };
    let mut audio_passthrough = saved_pass;
    let mut audio_exclusive = saved_audio_exclusive;
    let mut audio_channels = saved_chans;
    let mut log_level = saved_log_level;

    let mut ozone_platform = String::new();
    let log_file = cli.log_file;
    let mut disable_gpu_compositing = false;
    let mut remote_debugging_port: c_int = 0;

    if let Some(v) = cli.hwdec {
        hwdec = v;
    }
    if let Some(v) = cli.audio_passthrough {
        audio_passthrough = v;
    }
    if let Some(v) = cli.audio_channels {
        audio_channels = v;
    }
    if let Some(v) = cli.log_level {
        log_level = v;
    }
    if let Some(v) = cli.ozone_platform {
        ozone_platform = v;
    }
    if cli.audio_exclusive {
        audio_exclusive = true;
    }
    if cli.disable_gpu_compositing {
        disable_gpu_compositing = true;
    }
    if let Some(p) = cli.remote_debug_port {
        remote_debugging_port = p;
    }

    // Validate hwdec.
    if !jfn_mpv::is_valid_hwdec(&hwdec) {
        hwdec = mpv_hwdec_default;
    }

    // Normalize audio_passthrough (dts-hd subsumes dts).
    if !audio_passthrough.is_empty() {
        audio_passthrough = normalize_passthrough(&audio_passthrough);
    }

    StartupOptions {
        hwdec,
        audio_passthrough,
        audio_exclusive,
        audio_channels,
        log_level,
        ozone_platform,
        log_file,
        disable_gpu_compositing,
        remote_debugging_port,
        #[cfg(target_os = "linux")]
        platform: cli.platform,
        #[cfg(target_os = "linux")]
        platform_paint: cli.platform_paint,
    }
}

struct MpvInitOptions<'a> {
    backend_byte: u8,
    boot_geometry: &'a str,
    boot_force_position: bool,
    boot_window_max: bool,
    hwdec: &'a str,
    audio_passthrough: &'a str,
    audio_exclusive: bool,
    audio_channels: &'a str,
    mpv_log_level: &'a str,
}

fn init_mpv_handle(opts: MpvInitOptions<'_>) -> *mut jfn_mpv::sys::mpv_handle {
    let geometry_c = cs(opts.boot_geometry);
    let hwdec_c = cs(opts.hwdec);
    let user_agent_c = cs(&format!("JellyfinDesktop/{}", APP_VERSION_FULL));
    let passthrough_c = cs(opts.audio_passthrough);
    let channels_c = cs(opts.audio_channels);
    let mpv_log_level_c = cs(opts.mpv_log_level);
    let boot = jfn_mpv::boot::JfnMpvBoot {
        display_backend: opts.backend_byte,
        hwdec: hwdec_c.as_ptr(),
        user_agent: user_agent_c.as_ptr(),
        audio_passthrough: if opts.audio_passthrough.is_empty() {
            ptr::null()
        } else {
            passthrough_c.as_ptr()
        },
        audio_exclusive: opts.audio_exclusive,
        audio_channels: if opts.audio_channels.is_empty() {
            ptr::null()
        } else {
            channels_c.as_ptr()
        },
        geometry: geometry_c.as_ptr(),
        force_window_position: opts.boot_force_position,
        window_maximized_at_boot: opts.boot_window_max,
        mpv_log_level: mpv_log_level_c.as_ptr(),
        client_side_decorations: jfn_config::client_side_decorations(),
    };
    unsafe { jfn_mpv::boot::jfn_mpv_handle_init(&boot as *const _) }
}

fn wait_for_vo_window() -> Option<(i32, i32)> {
    let want_max = {
        let g = jfn_config::window_geometry();
        g.maximized
    };
    let wait_for_scale = cfg!(target_os = "linux") && plat().display() == DisplayBackend::Wayland;
    tracing::info!(target: "Main", "Waiting for mpv window...");

    #[cfg(target_os = "macos")]
    unsafe {
        jfn_mpv::api::jfn_mpv_set_wakeup_callback(
            jfn_macos::macos_mpv_wakeup_cb,
            std::ptr::null_mut(),
        );
    }

    let mut mw: i32 = 0;
    let mut mh: i32 = 0;
    let mut need_max = want_max;
    'wait: loop {
        // Drain everything mpv has queued without blocking. consume_vo_event
        // folds property changes into the ingest layer; if any drain step
        // observes a fatal event we bail out of jfn_app_main.
        loop {
            match jfn_mpv::api::wait_event_owned(0.0) {
                jfn_mpv::api::WaitEvent::None => {
                    break;
                }
                jfn_mpv::api::WaitEvent::LogMessage(m) => {
                    jfn_mpv::forward_log_to_tracing(&m);
                    continue;
                }
                jfn_mpv::api::WaitEvent::Event(
                    jfn_mpv::Event::Shutdown | jfn_mpv::Event::EndFile(_),
                ) => return None,
                jfn_mpv::api::WaitEvent::Event(event) => {
                    consume_vo_event(&event, &mut mw, &mut mh, &mut need_max);
                }
            }
        }
        if vo_ready(&mut mw, &mut mh, &need_max, wait_for_scale) {
            break 'wait;
        }
        // Block until the next mpv wakeup (or, on macOS, until the main
        // run loop services a source — e.g. the dispatch block posted by
        // the wakeup callback).
        #[cfg(target_os = "macos")]
        jfn_macos::macos_pump_block(60.0);
        #[cfg(not(target_os = "macos"))]
        {
            match jfn_mpv::api::wait_event_owned(-1.0) {
                jfn_mpv::api::WaitEvent::None => {}
                jfn_mpv::api::WaitEvent::LogMessage(m) => jfn_mpv::forward_log_to_tracing(&m),
                jfn_mpv::api::WaitEvent::Event(
                    jfn_mpv::Event::Shutdown | jfn_mpv::Event::EndFile(_),
                ) => return None,
                jfn_mpv::api::WaitEvent::Event(event) => {
                    consume_vo_event(&event, &mut mw, &mut mh, &mut need_max);
                }
            }
        }
    }

    #[cfg(target_os = "macos")]
    {
        // Drop the boot-time wakeup callback now that the VO is ready —
        // the regular event-loop thread set up later does its own drain.
        jfn_mpv::api::jfn_mpv_clear_wakeup_callback();
    }

    Some((mw, mh))
}

fn publish_device_profile(mpv_raw: *mut jfn_mpv::sys::mpv_handle) {
    let caps = unsafe { jfn_mpv::capabilities::query_raw(mpv_raw) };
    let decoders: Vec<jfn_jellyfin::Codec> = caps
        .decoders
        .into_iter()
        .map(|c| jfn_jellyfin::Codec {
            name: c.name,
            kind: match c.kind {
                jfn_mpv::capabilities::MediaKind::Video => jfn_jellyfin::MediaKind::Video,
                jfn_mpv::capabilities::MediaKind::Audio => jfn_jellyfin::MediaKind::Audio,
                jfn_mpv::capabilities::MediaKind::Subtitle => jfn_jellyfin::MediaKind::Subtitle,
            },
        })
        .collect();
    let force = jfn_config::force_transcoding();
    let profile = jfn_jellyfin::build_device_profile(
        &decoders,
        &caps.demuxers,
        "Jellyfin Desktop",
        APP_VERSION_FULL,
        force,
    );
    tracing::info!(target: "Main", "Device profile: {profile}");
    unsafe {
        jfn_cef::injection::jfn_cef_set_device_profile_json(
            profile.as_ptr() as *const _,
            profile.len(),
        );
    }
}

fn initialize_cef(ba: &BootArgs, ozone_platform: &str, use_shared_textures: bool) -> bool {
    jfn_cef::ffi::jfn_cef_set_log_severity(cef_severity_for_cef_filter());
    jfn_cef::ffi::jfn_cef_set_remote_debugging_port(ba.remote_debugging_port);
    jfn_cef::ffi::jfn_cef_set_disable_gpu_compositing(!use_shared_textures);
    #[cfg(target_os = "linux")]
    {
        if !ozone_platform.is_empty() {
            jfn_cef::ffi::jfn_cef_set_ozone_platform(ozone_platform);
        }
    }
    tracing::info!(target: "Main", "[FLOW] calling CefInitialize...");
    if !jfn_cef::ffi::jfn_cef_initialize() {
        tracing::error!(target: "Main", "CefInitialize failed");
        return false;
    }
    CEF_INITED.store(true, std::sync::atomic::Ordering::Release);
    tracing::info!(target: "Main", "[FLOW] CefInitialize returned ok");
    true
}

fn start_playback_coordination() -> bool {
    jfn_playback::ffi::jfn_playback_init();
    COORD_INITED.store(true, std::sync::atomic::Ordering::Release);

    jfn_playback::idle_inhibit_sink::jfn_playback_set_idle_inhibit_handler(Some(h_idle_inhibit));
    jfn_playback::theme_color_sink::jfn_playback_set_theme_video_mode_handler(Some(
        h_theme_video_mode,
    ));
    jfn_playback::exec_js::jfn_playback_set_web_exec_js_handler(Some(h_web_exec_js));
    jfn_playback::browser_sink::jfn_playback_set_browsers_size_handler(Some(h_browsers_set_size));
    jfn_playback::browser_sink::jfn_playback_set_browsers_refresh_rate_handler(Some(
        h_browsers_set_refresh_rate,
    ));

    #[cfg(target_os = "linux")]
    {
        let empty = cs("");
        unsafe { jfn_playback::mpris_sink::jfn_mpris_sink_start(empty.as_ptr()) };
    }
    #[cfg(target_os = "macos")]
    jfn_macos_sink::jfn_macos_sink_start();
    #[cfg(target_os = "windows")]
    jfn_windows_sink::jfn_windows_sink_start();

    jfn_playback::ingest_driver::jfn_playback_set_display_scale_handler(|s| {
        if s > 0.0 {
            jfn_cef::browsers::jfn_browsers_set_scale(s);
        }
    });
    jfn_playback::ingest_driver::jfn_playback_set_scale_provider(|| {
        let s = plat().get_scale();
        if s > 0.0 { s } else { 1.0 }
    });
    jfn_playback::ingest_driver::jfn_playback_set_fullscreen_handler(|fs| {
        plat().set_fullscreen(fs)
    });
    jfn_playback::ingest_driver::jfn_playback_set_shutdown_handler(|| {
        tracing::info!(target: "Main", "MPV_EVENT_SHUTDOWN received");
        jfn_playback::jfn_shutdown_initiate();
    });

    tracing::info!(target: "Main", "[FLOW] starting Rust-owned mpv event thread");
    if !jfn_playback::ingest_driver::jfn_playback_start_mpv_event_thread() {
        tracing::error!(target: "Main", "failed to start mpv event thread");
        return false;
    }
    true
}

fn shutdown_runtime(manager_thread: std::thread::JoinHandle<()>) {
    // Manager woke us, so its orchestration loop has returned. Join it before
    // any teardown so no posted task outlives the layer free below.
    let _ = manager_thread.join();

    jfn_color::theme::jfn_theme_color_shutdown();
    #[cfg(target_os = "macos")]
    jfn_macos_sink::jfn_macos_sink_stop();
    #[cfg(target_os = "windows")]
    jfn_windows_sink::jfn_windows_sink_stop();
    #[cfg(target_os = "linux")]
    jfn_playback::mpris_sink::jfn_mpris_sink_stop();

    jfn_playback::ingest_driver::jfn_playback_stop_mpv_event_thread();

    save_window_geometry_on_exit();
    jfn_config::settings_save();
    jfn_config::settings_shutdown_save_worker();

    jfn_cef::browsers::jfn_browsers_shutdown();
    jfn_cef::ffi::jfn_cef_shutdown();
    CEF_INITED.store(false, std::sync::atomic::Ordering::Release);

    plat().set_idle_inhibit(IdleInhibitLevel::None);

    plat().cleanup();
    PLATFORM_INITED.store(false, std::sync::atomic::Ordering::Release);

    jfn_playback::ffi::jfn_playback_shutdown();
    COORD_INITED.store(false, std::sync::atomic::Ordering::Release);
}

struct CefWindowMetrics {
    lw: c_int,
    lh: c_int,
    mw: c_int,
    mh: c_int,
    hz: f64,
}

fn sync_cef_window_metrics(
    mpv_raw: *mut jfn_mpv::sys::mpv_handle,
    mut mw: c_int,
    mut mh: c_int,
) -> CefWindowMetrics {
    let mut display_hidpi_scale: f64 = 0.0;
    unsafe {
        let name = cs("display-hidpi-scale");
        jfn_mpv::sys::mpv_get_property(
            mpv_raw,
            name.as_ptr(),
            jfn_mpv::sys::mpv_format::MPV_FORMAT_DOUBLE,
            &mut display_hidpi_scale as *mut f64 as *mut std::ffi::c_void,
        );
    }
    let mut fs_flag: c_int = 0;
    unsafe {
        let name = cs("fullscreen");
        jfn_mpv::sys::mpv_get_property(
            mpv_raw,
            name.as_ptr(),
            jfn_mpv::sys::mpv_format::MPV_FORMAT_FLAG,
            &mut fs_flag as *mut c_int as *mut std::ffi::c_void,
        );
    }
    jfn_playback::ingest_driver::jfn_playback_seed_display_hz_sync();
    let hz = jfn_playback::ingest_driver::jfn_playback_display_hz();
    tracing::info!(target: "Main",
        "[FLOW] display-hidpi-scale={display_hidpi_scale} fullscreen={fs_flag} display-hz={hz}");

    // Scale-correct the window size when live display scale differs from
    // saved. Skip while the compositor has the surface locked.
    let saved = jfn_config::window_geometry();
    let locked = fs_flag != 0 || jfn_playback::ingest_driver::jfn_playback_window_maximized();
    if !locked
        && display_hidpi_scale > 0.0
        && saved.scale > 0.0
        && (display_hidpi_scale - saved.scale as f64).abs() >= 0.01
    {
        let new_pw = (saved.logical_width as f64 * display_hidpi_scale).round() as c_int;
        let new_ph = (saved.logical_height as f64 * display_hidpi_scale).round() as c_int;
        // Only the size matters here; x/y are unused on the return.
        let clamped = plat().clamp_window_geometry(WindowGeometry {
            w: new_pw,
            h: new_ph,
            x: -1,
            y: -1,
        });
        let (new_pw, new_ph) = (clamped.w, clamped.h);
        let geom_str = format!("{new_pw}x{new_ph}");
        tracing::info!(target: "Main",
            "[FLOW] scale {:.3} -> {:.3}, resize to {}", saved.scale, display_hidpi_scale, geom_str);
        let g_c = cs(&geom_str);
        unsafe { jfn_mpv::api::jfn_mpv_set_geometry(g_c.as_ptr()) };
        mw = new_pw;
        mh = new_ph;
    }
    jfn_playback::ingest_driver::jfn_playback_set_window_pixels(mw, mh);

    let scale = if display_hidpi_scale > 0.0 {
        display_hidpi_scale as f32
    } else {
        plat().get_scale()
    };
    let lw = (mw as f32 / scale) as c_int;
    let lh = (mh as f32 / scale) as c_int;

    CefWindowMetrics { lw, lh, mw, mh, hz }
}

fn init_main_browser(
    lw: c_int,
    lh: c_int,
    mw: c_int,
    mh: c_int,
    hz: f64,
    use_shared_textures: bool,
) -> (std::thread::JoinHandle<()>, *mut jfn_cef::JfnCefLayer) {
    // Theme color init — must exist before main browser create (the
    // pre-loaded page fires its initial theme-color IPC at DOMContentLoaded).
    let titlebar_themed = jfn_config::titlebar_theme_color();
    unsafe {
        jfn_color::theme::jfn_theme_color_init(
            if titlebar_themed {
                Some(h_theme_set_titlebar)
            } else {
                None
            },
            Some(h_theme_set_mpv_bg),
        );
    }
    jfn_color::theme::jfn_theme_color_set_video_bg(video_bg_get());

    jfn_cef::browsers::jfn_browsers_init(lw, lh, mw, mh, hz, use_shared_textures);
    // Headless control-plane thread. Owns shutdown orchestration (close/drain
    // off the main thread + TID_UI) and is the seam for future routed work.
    let manager_thread = crate::manager::jfn_manager_start();
    jfn_playback::jfn_shutdown_set_handler(Some(h_shutdown_wake_manager));

    let web_kind = cs("web");
    let main_layer = unsafe { jfn_cef::browsers::jfn_browsers_create(web_kind.as_ptr()) };
    jfn_cef::business_web::jfn_web_init(main_layer);

    let server_url = jfn_config::server_url();
    tracing::info!(target: "Main",
        "[FLOW] CreateBrowser(main) url={server_url} lw={lw} lh={lh} pw={mw} ph={mh}");
    unsafe {
        jfn_cef::client::jfn_cef_layer_create(
            main_layer,
            server_url.as_ptr() as *const _,
            server_url.len(),
        );
    }
    tracing::info!(target: "Main", "[FLOW] CreateBrowser(main) call returned");

    tracing::info!(target: "Main", "[FLOW] jfn_overlay_init(main_layer)");
    jfn_cef::business_overlay::jfn_overlay_init(main_layer);
    tracing::info!(target: "Main", "[FLOW] jfn_overlay_init returned");

    (manager_thread, main_layer)
}

/// Subprocess dispatch + early-boot (settings/CLI/logging).
pub fn jfn_app_main() -> c_int {
    // Windows/macOS use a fixed display backend so the Platform must be
    // installed before CefExecuteProcess (subprocesses bail out of the
    // browser-process flow but may still query the platform).
    #[cfg(target_os = "windows")]
    {
        let p = jfn_windows::make_windows_platform();
        p.early_init();
        jfn_platform_abi::install(p);
    }
    #[cfg(target_os = "macos")]
    {
        let p = jfn_macos::make_macos_platform();
        p.early_init();
        jfn_platform_abi::install(p);
    }

    // 1. CEF subprocess dispatch: returns >= 0 in renderer/GPU/utility
    //    subprocesses (subprocess exit code), -1 in the browser process.
    let rc = jfn_cef::ffi::jfn_cef_start();
    if rc >= 0 {
        return rc;
    }

    // Path overrides must be applied before settings load and CEF
    // root_cache_path construction below.
    let cli = cli::Cli::parse();
    if cli.version {
        print_version();
        return 0;
    }
    if let Some(path) = &cli.config_dir {
        jfn_paths::set_config_dir_override(path.into());
    }
    if let Some(path) = &cli.cache_dir {
        jfn_paths::set_cache_dir_override(path.into());
    }

    // 3. Settings init + load.
    let settings_path = jfn_paths::config_dir().join("settings.json");
    jfn_config::settings_init(&settings_path);
    jfn_config::settings_load();

    // 4-6. Resolve settings-backed runtime options and CLI overrides.
    let opts = resolve_startup_options(cli);

    // 7. Resolve logging defaults and initialise tracing output.
    init_logging(opts.log_file, &opts.log_level);

    // 9. Linux: pick display backend, populate g_platform, run early_init,
    //    register the platform-ops vtable with the Rust-side jfn-cef.
    //    Windows/macOS: g_platform was populated by main() before jfn_app_main
    //    returned (we ran before CefExecuteProcess on those platforms).
    #[cfg(target_os = "linux")]
    install_linux_platform(opts.platform, opts.platform_paint);

    // 10. Install signal handler (Unix) / Windows ConsoleCtrl handler.
    install_signal_handler();

    // 11. Single-instance check.
    if !init_single_instance() {
        return 0;
    }

    // 12-13. Configure mpv env and Linux/Wayland proxy before mpv init.
    setup_mpv_environment();

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
    let backend_byte: u8 = plat().display() as u8;
    let raw = init_mpv_handle(MpvInitOptions {
        backend_byte,
        boot_geometry: &boot_geometry,
        boot_force_position,
        boot_window_max,
        hwdec: &opts.hwdec,
        audio_passthrough: &opts.audio_passthrough,
        audio_exclusive: opts.audio_exclusive,
        audio_channels: &opts.audio_channels,
        mpv_log_level,
    });
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
    video_bg_set(user_bg);
    {
        let hex = format!("#{:06x}", user_bg);
        tracing::info!(target: "Main", "video bg captured: {hex}");
    }
    let startup_bg = cs("#101010");
    unsafe { jfn_mpv::api::jfn_mpv_set_background_color_hex(startup_bg.as_ptr()) };

    // 19. Log mpv-version + ffmpeg-version.
    log_mpv_versions();

    // 20. Re-bind CLOSE_WIN -> quit. input-default-bindings=no removes
    //     all builtin bindings including this one; the WM close button
    //     needs it back.
    install_mpv_close_binding(raw);

    // 21. Wait for the VO window. Event-driven: drain mpv's queued events
    //     into the ingest layer, re-check the readiness gate (OSD pixels
    //     non-zero, maximize state matches request, Wayland scale known),
    //     and block until the next wakeup if not ready.
    //
    //     macOS pumps NSEvents + CFRunLoop sources between drains, because
    //     the main thread must service AppKit while waiting. A mpv wakeup
    //     callback dispatches a no-op block onto the main queue so the
    //     CFRunLoop returns when libmpv has new events. Linux/Windows can
    //     simply block in `mpv_wait_event(-1.0)`; the Wayland scale-known
    //     path posts `mpv_wakeup` from the wl-proxy thread to unblock.
    let Some((mw, mh)) = wait_for_vo_window() else {
        return 0;
    };

    store_vo_size(mw, mh);

    // 22. run_with_cef body + post-run mpv terminate. From here on
    //     jfn_app_main fully owns the rest of the process lifetime.
    let boot_args = BootArgs {
        ozone_platform: opts.ozone_platform,
        disable_gpu_compositing: opts.disable_gpu_compositing,
        remote_debugging_port: opts.remote_debugging_port,
    };
    let rc = unsafe { run_with_cef(&boot_args, mw, mh) };
    if rc != 0 {
        return rc;
    }

    // 23. mpv terminate.
    terminate_mpv_handle();

    // 24. Boot-resource teardown + platform post-window cleanup.
    jfn_app_teardown();
    plat().post_window_cleanup();

    0
}

// =====================================================================
// Signal handler + listener guard + wlproxy lifetime
// =====================================================================

#[cfg(unix)]
static SIGNAL_GUARD: OnceLock<crate::signal_guard::SignalGuard> = OnceLock::new();

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
        let g = unsafe { crate::signal_guard::install(on_shutdown_signal) };
        let _ = SIGNAL_GUARD.set(g);
    }
    #[cfg(windows)]
    {
        unsafe extern "system" {
            fn SetConsoleCtrlHandler(
                handler: unsafe extern "system" fn(u32) -> i32,
                add: i32,
            ) -> i32;
        }
        unsafe { SetConsoleCtrlHandler(console_ctrl_handler, 1) };
    }
}

static LISTENER_GUARD: OnceLock<ListenerGuardSlot> = OnceLock::new();

struct ListenerGuardSlot;
impl Drop for ListenerGuardSlot {
    fn drop(&mut self) {
        crate::single_instance::stop_listener();
    }
}
unsafe impl Send for ListenerGuardSlot {}
unsafe impl Sync for ListenerGuardSlot {}

fn install_listener_guard() {
    let _ = LISTENER_GUARD.set(ListenerGuardSlot);
}

#[cfg(target_os = "linux")]
use jfn_wayland::proxy::jfn_wl_register_proxy_callbacks;
#[cfg(target_os = "linux")]
use jfn_wlproxy::{jfn_wlproxy_display_name, jfn_wlproxy_start, jfn_wlproxy_stop};

#[cfg(target_os = "linux")]
static WLPROXY: OnceLock<WlproxySlot> = OnceLock::new();

#[cfg(target_os = "linux")]
struct WlproxySlot(*mut jfn_wlproxy::Proxy);
#[cfg(target_os = "linux")]
unsafe impl Send for WlproxySlot {}
#[cfg(target_os = "linux")]
unsafe impl Sync for WlproxySlot {}

#[cfg(target_os = "linux")]
unsafe fn start_wlproxy() {
    let p = jfn_wlproxy_start();
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
    let disp = unsafe { CStr::from_ptr(disp_p) }
        .to_string_lossy()
        .into_owned();
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
    jfn_wl_register_proxy_callbacks();
    let _ = WLPROXY.set(WlproxySlot(p));
}

// =====================================================================
// mpv boot helpers + VO wait loop
// =====================================================================

const DEFAULT_LOGICAL_WIDTH: i32 = 1600;
const DEFAULT_LOGICAL_HEIGHT: i32 = 900;

fn compute_boot_geometry() -> (String, bool, bool) {
    let g = jfn_config::window_geometry();
    let mut x = g.x;
    let mut y = g.y;
    let scale = plat().get_display_scale(x, y);
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
    let clamped = plat().clamp_window_geometry(WindowGeometry { w, h, x, y });
    w = clamped.w;
    h = clamped.h;
    x = clamped.x;
    y = clamped.y;
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
    let e = jfn_logging::log_enabled;
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

const JFN_OBSERVE_WINDOW_MAX: u64 = 11;

fn current_macos_logical_size() -> Option<(i32, i32)> {
    #[cfg(target_os = "macos")]
    {
        let mut w: c_int = 0;
        let mut h: c_int = 0;
        if jfn_macos::jfn_macos_query_logical_content_size(&mut w, &mut h) {
            Some((w, h))
        } else {
            None
        }
    }
    #[cfg(not(target_os = "macos"))]
    {
        None
    }
}

fn consume_vo_event(event: &jfn_mpv::Event, mw: &mut i32, mh: &mut i32, need_max: &mut bool) {
    let scale_raw = plat().get_scale();
    let scale = if scale_raw > 0.0 { scale_raw } else { 1.0 };
    jfn_playback::ingest_driver::jfn_playback_ingest_mpv_event_owned(
        event,
        scale,
        current_macos_logical_size(),
    );
    if let jfn_mpv::Event::PropertyChange { id, .. } = event
        && *id == JFN_OBSERVE_WINDOW_MAX
        && jfn_playback::ingest_driver::jfn_playback_window_maximized()
    {
        *need_max = false;
    }
    let pw = jfn_playback::ingest_driver::jfn_playback_osd_pw();
    let ph = jfn_playback::ingest_driver::jfn_playback_osd_ph();
    if pw > 0 && ph > 0 {
        *mw = pw;
        *mh = ph;
    }
}

/// Boot-time VO readiness gate: OSD pixels reported, maximize state
/// matches request (if requested), Wayland scale known (if applicable).
/// Reads OSD pixels directly from the ingest layer (rather than the
/// caller's running `mw`) so a value that landed via the wlproxy synthetic
/// path before the loop entered is still observed.
fn vo_ready(mw: &mut i32, mh: &mut i32, need_max: &bool, wait_for_scale: bool) -> bool {
    let pw = jfn_playback::ingest_driver::jfn_playback_osd_pw();
    let ph = jfn_playback::ingest_driver::jfn_playback_osd_ph();
    if pw > 0 && ph > 0 {
        *mw = pw;
        *mh = ph;
    }
    #[cfg(target_os = "linux")]
    let scale_ready = !wait_for_scale || jfn_wayland::proxy::jfn_wl_scale_known();
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

// =====================================================================
// run_with_cef body — Rust port
// =====================================================================

const LOG_CEF: u8 = 2;
const LOG_SEVERITY_VERBOSE: c_int = -1;
const LOG_SEVERITY_INFO: c_int = 0;
const LOG_SEVERITY_WARNING: c_int = 1;
const LOG_SEVERITY_ERROR: c_int = 2;

fn cef_severity_for_cef_filter() -> c_int {
    // Map LOG_CEF level to CEF severity:
    //   Trace/Debug -> VERBOSE, Info -> INFO, Warn -> WARNING, Error -> ERROR.
    let e = jfn_logging::log_enabled;
    if e(LOG_CEF, LEVEL_TRACE) || e(LOG_CEF, LEVEL_DEBUG) {
        LOG_SEVERITY_VERBOSE
    } else if e(LOG_CEF, LEVEL_INFO) {
        LOG_SEVERITY_INFO
    } else if e(LOG_CEF, LEVEL_WARN) {
        LOG_SEVERITY_WARNING
    } else {
        LOG_SEVERITY_ERROR
    }
}

// Handler thunks installed via jfn_playback_set_*_handler. They capture
// nothing (Rust function items are 'static) and forward to the platform
// backend / jfn-cef.

extern "C" fn h_idle_inhibit(level: u32) {
    let lvl = match level {
        1 => IdleInhibitLevel::System,
        2 => IdleInhibitLevel::Display,
        _ => IdleInhibitLevel::None,
    };
    plat().set_idle_inhibit(lvl);
}
extern "C" fn h_theme_video_mode(active: bool) {
    jfn_color::theme::jfn_theme_color_set_video_mode(active);
}
extern "C" fn h_web_exec_js(js: *const c_char) {
    if !js.is_null() {
        unsafe { jfn_cef::business_web::jfn_web_exec_js(js) };
    }
}
extern "C" fn h_browsers_set_size(lw: i32, lh: i32, pw: i32, ph: i32) {
    jfn_cef::browsers::jfn_browsers_set_size(lw, lh, pw, ph);
}
extern "C" fn h_browsers_set_refresh_rate(hz: f64) {
    tracing::info!(target: "Main", "Display refresh rate changed: {hz} Hz");
    jfn_cef::browsers::jfn_browsers_set_refresh_rate(hz);
}
extern "C" fn h_theme_set_titlebar(rgb: u32) {
    plat().set_theme_color(rgb);
}
extern "C" fn h_theme_set_mpv_bg(hex: *const c_char) {
    unsafe { jfn_mpv::api::jfn_mpv_set_background_color_hex(hex) };
}

fn h_shutdown_wake_manager() {
    // Runs inline on whichever thread called jfn_shutdown_initiate (signal
    // handler, CEF dispatch, input thread, …). Signal-only by contract: just
    // wake the manager, which orchestrates the close/drain off-thread. Never
    // close a browser or wake the main loop here — that would reenter CEF or
    // race the drain.
    crate::manager::jfn_manager_notify_shutdown();
}

/// Owns the run_with_cef body — invoked once by `jfn_app_main`.
unsafe fn run_with_cef(ba: &BootArgs, mw: c_int, mh: c_int) -> c_int {
    // 1. Resolve final ozone_platform + write into g_platform.cef_ozone_platform.
    #[cfg(target_os = "linux")]
    let ozone_platform = {
        let mut p = ba.ozone_platform.clone();
        if p.is_empty() {
            p = if plat().display() == DisplayBackend::Wayland {
                "wayland".to_string()
            } else {
                "x11".to_string()
            };
        }
        p
    };
    #[cfg(not(target_os = "linux"))]
    let ozone_platform = ba.ozone_platform.clone();
    plat().set_cef_ozone_platform(&ozone_platform);

    // 2. Platform init (PlatformScope). Cleanup happens in jfn_app_teardown.
    let mpv_raw = jfn_mpv::boot::jfn_mpv_handle_get();
    let platform_ok = plat().init(mpv_raw as *mut std::ffi::c_void);
    if !platform_ok {
        tracing::error!(target: "Main", "Platform init failed");
        return 1;
    }
    tracing::info!(target: "Main", "Platform init ok");
    PLATFORM_INITED.store(true, std::sync::atomic::Ordering::Release);

    // 3. Apply titlebar theme color before CefInitialize so the window doesn't
    //    sit with the system default palette during init.
    if jfn_config::titlebar_theme_color() {
        plat().set_theme_color(0x101010);
    }

    // 4. Build device profile. Must run after VO-init wait — sync mpv API
    //    calls would deadlock against core_thread on macOS.
    publish_device_profile(mpv_raw);

    // 5. CEF init flags + initialise.
    let use_shared_textures = plat().shared_texture_supported() && !ba.disable_gpu_compositing;
    if !initialize_cef(ba, &ozone_platform, use_shared_textures) {
        return 1;
    }

    // 6-7. Read display-hidpi-scale/fullscreen state and sync CEF window metrics.
    let metrics = sync_cef_window_metrics(mpv_raw, mw, mh);

    // 8-9. Theme setup, browser registry, manager, main browser, overlay/web init.
    let (manager_thread, main_layer) = init_main_browser(
        metrics.lw,
        metrics.lh,
        metrics.mw,
        metrics.mh,
        metrics.hz,
        use_shared_textures,
    );

    // 10-13. Playback coordinator, media sink, handlers, and mpv event thread.
    if !start_playback_coordination() {
        return 1;
    }

    // 14. Wait for the main browser to finish loading (non-macOS).
    #[cfg(not(target_os = "macos"))]
    unsafe {
        jfn_cef::client::jfn_cef_layer_wait_for_load(main_layer)
    };
    tracing::info!(target: "Main", "Main browser loaded");

    tracing::info!(target: "Main", "[FLOW] Running — about to enter run_main_loop");

    // 15. Park the main thread until the manager has closed + drained every
    //     browser, at which point it calls plat().wake_main_loop() to release
    //     us. Unified across platforms: macOS parks in [NSApp run] (whose
    //     pump runs the posted close + OnBeforeClose while the manager waits);
    //     other platforms park on the Condvar main-park. Exit is driven by the
    //     shutdown signal (routed through the manager), never by transient
    //     browser-close state when the overlay resets the main layer.
    plat().run_main_loop();
    tracing::info!(target: "Main", "[FLOW] run_main_loop returned — browsers drained, running teardown");

    // 16-21. Shutdown drain and runtime teardown.
    shutdown_runtime(manager_thread);

    0
}

static PLATFORM_INITED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static CEF_INITED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static COORD_INITED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

fn save_window_geometry_on_exit() {
    let fs = jfn_playback::ingest_driver::jfn_playback_fullscreen();
    let max = jfn_playback::ingest_driver::jfn_playback_window_maximized();

    let saved = jfn_config::window_geometry();

    if fs {
        let mut g = saved;
        g.maximized = jfn_playback::browser_sink::jfn_playback_was_maximized_before_fullscreen();
        jfn_config::set_window_geometry(g);
    } else if max {
        let mut g = saved;
        g.maximized = true;
        jfn_config::set_window_geometry(g);
    } else {
        let mut pw = jfn_playback::ingest_driver::jfn_playback_window_pw();
        let mut ph = jfn_playback::ingest_driver::jfn_playback_window_ph();
        if pw <= 0 || ph <= 0 {
            pw = jfn_playback::ingest_driver::jfn_playback_osd_pw();
            ph = jfn_playback::ingest_driver::jfn_playback_osd_ph();
        }
        if pw > 0 && ph > 0 {
            let scale_raw = plat().get_scale();
            let win_scale = if scale_raw > 0.0 { scale_raw } else { 1.0 };
            let pos = plat().query_window_position();
            let wx = pos.map_or(-1, |p| p.x);
            let wy = pos.map_or(-1, |p| p.y);
            let g = jfn_config::JfnWindowGeometry {
                width: pw,
                height: ph,
                scale: win_scale,
                logical_width: (pw as f32 / win_scale).round() as i32,
                logical_height: (ph as f32 / win_scale).round() as i32,
                maximized: false,
                x: wx,
                y: wy,
            };
            jfn_config::set_window_geometry(g);
        }
    }
}

/// Tear down boot-owned resources at process exit (wlproxy,
/// single-instance listener).
pub fn jfn_app_teardown() {
    #[cfg(target_os = "linux")]
    {
        if let Some(slot) = WLPROXY.get() {
            unsafe { jfn_wlproxy_stop(slot.0) };
        }
    }
    // Single-instance listener is dropped via the OnceLock at process exit;
    // no explicit teardown call needed here. SignalGuard slot stays until
    // exit and restores the original disposition via its Drop impl.
}
