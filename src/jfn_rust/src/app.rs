//! Process entry point. [`jfn_app_main`] owns the full main loop and
//! returns the exit code.

use std::ffi::{CStr, CString, c_char, c_int};
use std::ptr;
use std::sync::OnceLock;

use clap::Parser;
use jfn_cef::{APP_CEF_VERSION, APP_VERSION_FULL};
use jfn_platform_abi::{IdleInhibitLevel, LogicalSize, Platform, WindowGeometry};

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
        "jellium-desktop {}\n\nCEF {}\n",
        APP_VERSION_FULL, APP_CEF_VERSION
    );
    use std::io::Write;
    let _ = std::io::stdout().flush();
    jfn_mpv::probe::jfn_mpv_print_version_info();
}

fn init_logging(log_file: Option<String>, log_level: &str) {
    let log_path = log_file.unwrap_or_else(|| {
        jfn_paths::default_log_file()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default()
    });

    let filter = if log_level.is_empty() {
        DEFAULT_LOG_FILTER.to_string()
    } else {
        log_level.to_string()
    };
    jfn_logging::jfn_log_init(&log_path, &filter);

    tracing::info!(target: "Main", "jellium-desktop {APP_VERSION_FULL}");
    tracing::info!(target: "Main", "CEF {APP_CEF_VERSION}");
    if !log_path.is_empty() {
        tracing::info!(target: "Main", "Log file: {log_path}");
    }
}

fn init_single_instance() -> bool {
    let id = crate::instance_id::instance_id();
    if plat().single_instance_try_signal(&id) {
        tracing::info!(target: "Main", "Signaled existing instance, exiting");
        return false;
    }
    let ok = plat().single_instance_start_listener(
        &id,
        Box::new(|_token: &str| {
            // TODO: raise window via xdg-activation
        }),
    );
    if !ok {
        tracing::warn!(target: "Main", "Single-instance listener failed to start");
    }
    install_listener_guard();
    true
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

fn install_mpv_close_binding(raw: *mut jfn_mpv::sys::mpv_handle) {
    let kb = cs("keybind");
    let name = cs("CLOSE_WIN");
    let action = cs("quit");
    let argv = [kb.as_ptr(), name.as_ptr(), action.as_ptr(), ptr::null()];
    unsafe { jfn_mpv::sys::mpv_command(raw, argv.as_ptr() as *mut *const c_char) };
}

fn setup_mpv_environment() {
    let mpv_home = jfn_paths::mpv_home();
    unsafe {
        std::env::set_var("MPV_HOME", &mpv_home);
    }

    plat()
        .mpv_host()
        .prepare(jfn_config::window_decorations_mode());
}

struct StartupOptions {
    hwdec: String,
    audio_passthrough: String,
    audio_exclusive: bool,
    audio_channels: String,
    log_level: String,
    log_file: Option<String>,
    disable_gpu_compositing: bool,
    remote_debugging_port: c_int,
}

fn resolve_startup_options(cli: &cli::Cli) -> StartupOptions {
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

    let log_file = cli.log_file.clone();
    let mut disable_gpu_compositing = false;
    let mut remote_debugging_port: c_int = 0;

    if let Some(v) = cli.hwdec.clone() {
        hwdec = v;
    }
    if let Some(v) = cli.audio_passthrough.clone() {
        audio_passthrough = v;
    }
    if let Some(v) = cli.audio_channels.clone() {
        audio_channels = v;
    }
    if let Some(v) = cli.log_level.clone() {
        log_level = v;
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

    if !jfn_mpv::is_valid_hwdec(&hwdec) {
        hwdec = mpv_hwdec_default;
    }

    if !audio_passthrough.is_empty() {
        audio_passthrough = normalize_passthrough(&audio_passthrough);
    }

    StartupOptions {
        hwdec,
        audio_passthrough,
        audio_exclusive,
        audio_channels,
        log_level,
        log_file,
        disable_gpu_compositing,
        remote_debugging_port,
    }
}

struct MpvInitOptions<'a> {
    backend_byte: u8,
    boot_geometry: Option<&'a str>,
    boot_force_position: bool,
    boot_window_max: bool,
    hwdec: &'a str,
    audio_passthrough: &'a str,
    audio_exclusive: bool,
    audio_channels: &'a str,
    mpv_log_level: &'a str,
}

fn init_mpv_handle(opts: MpvInitOptions<'_>) -> *mut jfn_mpv::sys::mpv_handle {
    let geometry_c = opts.boot_geometry.map(cs);
    let hwdec_c = cs(opts.hwdec);
    let user_agent_c = cs(&format!("JelliumDesktop/{}", APP_VERSION_FULL));
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
        geometry: geometry_c.as_ref().map_or(ptr::null(), |c| c.as_ptr()),
        force_window_position: opts.boot_force_position,
        window_maximized_at_boot: opts.boot_window_max,
        mpv_log_level: mpv_log_level_c.as_ptr(),
        client_side_decorations: jfn_config::client_side_decorations(),
    };
    unsafe { jfn_mpv::boot::jfn_mpv_handle_init(&boot as *const _) }
}

/// Blocks until the window source has a usable extent. Returns false on
/// a fatal mpv event (shutdown before the VO came up).
fn wait_for_vo_window() -> bool {
    let want_max = {
        let g = jfn_config::window_geometry();
        g.maximized
    };
    tracing::info!(target: "Main", "Waiting for mpv window...");

    let mut need_max = want_max;
    let mut fatal = false;

    // The platform owns the wait strategy; this pump owns all mpv event
    // handling. It drains everything mpv has queued without blocking —
    // consume_vo_event folds property changes into the ingest layer; a
    // fatal event bails out of jfn_app_main — then, when the platform's
    // strategy is the generic blocking wait (`may_block`), parks in mpv
    // until the next wakeup.
    plat().mpv_host().run_vo_wait(&mut |may_block| {
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
                ) => {
                    fatal = true;
                    return false;
                }
                jfn_mpv::api::WaitEvent::Event(event) => {
                    consume_vo_event(&event);
                }
            }
        }
        if vo_ready(&mut need_max) {
            return false;
        }
        if may_block {
            match jfn_mpv::api::wait_event_owned(-1.0) {
                jfn_mpv::api::WaitEvent::None => {}
                jfn_mpv::api::WaitEvent::LogMessage(m) => jfn_mpv::forward_log_to_tracing(&m),
                jfn_mpv::api::WaitEvent::Event(
                    jfn_mpv::Event::Shutdown | jfn_mpv::Event::EndFile(_),
                ) => {
                    fatal = true;
                    return false;
                }
                jfn_mpv::api::WaitEvent::Event(event) => {
                    consume_vo_event(&event);
                }
            }
        }
        true
    });

    !fatal
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
        "Jellium Desktop",
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

fn initialize_cef(ba: &BootArgs, use_shared_textures: bool) -> bool {
    jfn_cef::ffi::jfn_cef_set_log_severity(cef_severity_for_cef_filter());
    jfn_cef::ffi::jfn_cef_set_remote_debugging_port(ba.remote_debugging_port);
    jfn_cef::ffi::jfn_cef_set_disable_gpu_compositing(!use_shared_textures);
    jfn_cef::ffi::jfn_cef_set_platform_switches(plat().display());
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
    jfn_playback::browser_sink::jfn_playback_set_window_wakeup_handler(|| {
        crate::browser_size::sync_browsers_to_window();
    });
    jfn_playback::browser_sink::jfn_playback_set_browsers_refresh_rate_handler(Some(
        h_browsers_set_refresh_rate,
    ));

    plat().media_session().start();

    jfn_playback::ingest_driver::jfn_playback_set_display_scale_handler(|_| {
        crate::browser_size::sync_browsers_to_window();
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

    // The sync in init_main_browser ran before the coordinator existed, so
    // its window-mode posts were dropped; reconcile now that inputs land.
    crate::browser_size::sync_browsers_to_window();
    true
}

fn shutdown_runtime(manager_thread: std::thread::JoinHandle<()>) {
    // Persist before the joins below: they can block on a VO-teardown
    // roundtrip, and a hang there must not cost the window geometry.
    crate::window_geometry::controller().persist();
    jfn_config::settings_save();

    // Join before any teardown so no posted task outlives the layer free below.
    let _ = manager_thread.join();

    // Sever host↔mpv links that could deadlock the teardown below once
    // CEF threads start dying.
    plat().mpv_host().detach();

    jfn_color::theme::jfn_theme_color_shutdown();
    plat().media_session().stop();

    jfn_playback::ingest_driver::jfn_playback_stop_mpv_event_thread();

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

/// Boot-time mpv-size reconcile and extent-cell seed. Returns the display
/// refresh rate for browser init.
fn sync_cef_window_metrics(mpv_raw: *mut jfn_mpv::sys::mpv_handle) -> f64 {
    let (mut mw, mut mh) = boot_window_size().unwrap_or((0, 0));
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

    let saved = jfn_config::window_geometry();
    let locked = fs_flag != 0 || jfn_playback::ingest_driver::jfn_playback_window_maximized();
    if let Some(physical) = plat().reconcile_mpv_size(
        display_hidpi_scale,
        saved.scale,
        LogicalSize {
            w: saved.logical_width,
            h: saved.logical_height,
        },
        locked,
    ) {
        let clamped = plat().clamp_window_geometry(WindowGeometry {
            w: physical.w,
            h: physical.h,
            position: None,
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
    if mw > 0 && mh > 0 {
        jfn_playback::ingest_driver::jfn_playback_seed_window_extent(
            mw,
            mh,
            plat().effective_scale(display_hidpi_scale),
        );
    }

    hz
}

fn init_main_browser(
    hz: f64,
    use_shared_textures: bool,
) -> (std::thread::JoinHandle<()>, *mut jfn_cef::JfnCefLayer) {
    // Must run before main browser create: the pre-loaded page fires its
    // initial theme-color IPC at DOMContentLoaded.
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

    jfn_cef::browsers::jfn_browsers_init(hz, use_shared_textures);
    // jfn_browsers_create sizes new layers from the cache this sync seeds.
    crate::browser_size::sync_browsers_to_window();
    let manager_thread = crate::manager::jfn_manager_start();
    jfn_playback::jfn_shutdown_set_handler(Some(h_shutdown_wake_manager));

    let web_kind = cs("web");
    let main_layer = unsafe { jfn_cef::browsers::jfn_browsers_create(web_kind.as_ptr()) };
    jfn_cef::business_web::jfn_web_init(main_layer);

    let server_url = jfn_config::server_url();
    tracing::info!(target: "Main", "[FLOW] CreateBrowser(main) url={server_url}");
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

pub fn jfn_app_main() -> c_int {
    crate::platform_install::install_early();

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

    let settings_path = jfn_paths::config_dir().join("settings.json");
    jfn_config::settings_init(&settings_path);
    jfn_config::settings_load();

    let opts = resolve_startup_options(&cli);

    init_logging(opts.log_file, &opts.log_level);

    crate::platform_install::install_from_cli(&cli);

    let _ = crate::window_geometry::controller();

    plat().install_shutdown_handler(jfn_playback::jfn_shutdown_initiate);

    if !init_single_instance() {
        return 0;
    }

    setup_mpv_environment();

    let boot = crate::window_geometry::controller().boot();
    plat().apply_boot_geometry(&boot);

    let mpv_log_level = mpv_log_level_from_filter();

    // mpv's --geometry takes physical pixels (see m_geometry_apply in
    // third_party/mpv/options/m_option.c).
    let backend_byte: u8 = plat().display() as u8;
    let boot_mpv_geometry = plat().boot_mpv_geometry(&boot);
    let raw = init_mpv_handle(MpvInitOptions {
        backend_byte,
        boot_geometry: boot_mpv_geometry.as_deref(),
        boot_force_position: boot.force_position(),
        boot_window_max: boot.maximized(),
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

    if !jfn_playback::ingest_driver::jfn_playback_observe_mpv_properties(backend_byte) {
        tracing::error!(target: "Main", "observe_mpv_properties failed");
        return 1;
    }

    // force-window=yes (not "immediate") defers VO creation so the user's
    // mpv.conf color never flashes before this override is applied.
    let user_bg = jfn_mpv::api::jfn_mpv_get_background_color();
    video_bg_set(user_bg);
    {
        let hex = format!("#{:06x}", user_bg);
        tracing::info!(target: "Main", "video bg captured: {hex}");
    }
    let startup_bg = cs("#101010");
    unsafe { jfn_mpv::api::jfn_mpv_set_background_color_hex(startup_bg.as_ptr()) };

    log_mpv_versions();

    // input-default-bindings=no drops the builtin CLOSE_WIN -> quit binding;
    // the WM close button needs it back.
    install_mpv_close_binding(raw);

    plat().mpv_host().ensure_host_window();

    if !wait_for_vo_window() {
        return 0;
    }

    let boot_args = BootArgs {
        disable_gpu_compositing: opts.disable_gpu_compositing,
        remote_debugging_port: opts.remote_debugging_port,
    };
    let rc = unsafe { run_with_cef(&boot_args) };
    if rc != 0 {
        return rc;
    }

    // macOS must run TerminateDestroy off the main thread (mpv's VO uninit
    // does DispatchQueue.main.sync); run_blocking keeps main pumping.
    plat().run_blocking(Box::new(jfn_mpv::boot::jfn_mpv_handle_terminate));

    plat().post_window_cleanup();

    0
}

// =====================================================================
// Single-instance listener guard
// =====================================================================

static LISTENER_GUARD: OnceLock<ListenerGuardSlot> = OnceLock::new();

struct ListenerGuardSlot;
impl Drop for ListenerGuardSlot {
    fn drop(&mut self) {
        plat().single_instance_stop(&crate::instance_id::instance_id());
    }
}
unsafe impl Send for ListenerGuardSlot {}
unsafe impl Sync for ListenerGuardSlot {}

fn install_listener_guard() {
    let _ = LISTENER_GUARD.set(ListenerGuardSlot);
}

// =====================================================================
// mpv boot helpers + VO wait loop
// =====================================================================

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

fn boot_window_size() -> Option<(i32, i32)> {
    crate::window_geometry::controller()
        .source()
        .snapshot()
        .extent
        .map(|e| (e.physical().w, e.physical().h))
}

fn consume_vo_event(event: &jfn_mpv::Event) {
    let scale_raw = plat().get_scale();
    let scale = if scale_raw > 0.0 { scale_raw } else { 1.0 };
    jfn_playback::ingest_driver::jfn_playback_ingest_mpv_event_owned(
        event,
        scale,
        plat().mpv_host().logical_content_size(),
    );
}

fn vo_ready(need_max: &mut bool) -> bool {
    let reported_max = plat()
        .mpv_host()
        .window_maximized()
        .unwrap_or_else(jfn_playback::ingest_driver::jfn_playback_window_maximized);
    if reported_max {
        *need_max = false;
    }
    boot_window_size().is_some() && !*need_max && plat().mpv_host().host_ready()
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
unsafe fn run_with_cef(ba: &BootArgs) -> c_int {
    // 2. Platform init (PlatformScope). Cleanup happens in shutdown_runtime.
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
    if !initialize_cef(ba, use_shared_textures) {
        return 1;
    }

    let hz = sync_cef_window_metrics(mpv_raw);

    let (manager_thread, main_layer) = init_main_browser(hz, use_shared_textures);

    if !start_playback_coordination() {
        return 1;
    }

    // 14. Wait for the main browser to finish loading. Skipped when the
    //     platform pumps CEF itself (external pump on the main thread):
    //     blocking main here would starve the pump and never load.
    if plat().cef_host().is_none() {
        unsafe { jfn_cef::client::jfn_cef_layer_wait_for_load(main_layer) };
    }
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

    shutdown_runtime(manager_thread);

    0
}

static PLATFORM_INITED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static CEF_INITED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static COORD_INITED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

// Single-instance listener is dropped via its OnceLock at process exit;
// signal-disposition restore lives in `shutdown_signal`. Wayland host
// teardown lives in the backend's `post_window_cleanup`.
