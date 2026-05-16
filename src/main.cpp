// jellyfin-desktop Linux: native mpv VO + CEF browser overlays.
//
// Threading:
//   mpv digest thread: mpv_wait_event -> normalize -> fan out to consumer queue
//   CEF consumer thread: drains queue -> execJs/resize
//   CEF render thread: multi_threaded_message_loop (autonomous)
//   Input thread: Wayland dispatch -> CEF key/mouse -> mpv async
//   mpv VO thread: configure/close callbacks -> surface ops
//   Main thread: startup -> waitForClose -> cleanup

#include "version.h"
#include "cli.h"
#include "common.h"
#include "cef/cef_app.h"
#include "cef/cef_client.h"
#include "browser/browsers.h"
#include "browser/web_browser.h"
#include "browser/overlay_browser.h"
#include "mpv/event.h"
#include "mpv/options.h"
#include "mpv/capabilities.h"
#include "jellyfin/device_profile.h"
#include "paths/paths.h"
#include "settings.h"
#include "theme_color.h"

#include "playback/coordinator.h"
#include "playback/sinks.h"
#if defined(__APPLE__)
#include "playback/sinks/macos/macos_sink.h"
#elif defined(_WIN32)
#include "playback/sinks/windows/windows_sink.h"
#else
#include "playback/sinks/mpris/mpris_sink.h"
#endif

#include "logging.h"
#include "signal_guard.h"
#include "shutdown.h"
#include "event_dispatcher.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <signal.h>
#else
#include "single_instance.h"
#endif

#include "include/cef_version.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#ifndef _WIN32
#include <poll.h>
#endif

// =====================================================================
// Globals
// =====================================================================

MpvHandle g_mpv;
Color g_video_bg;

PlaybackCoordinator* g_playback_coord = nullptr;
ThemeColor* g_theme_color = nullptr;

Platform g_platform{};
WebBrowser* g_web_browser = nullptr;
// g_browsers is defined in src/browser/browsers.cpp.

static void log_mpv_message(const mpv_event_log_message* msg) {
    switch (msg->log_level) {
    case MPV_LOG_LEVEL_FATAL:
    case MPV_LOG_LEVEL_ERROR:
        LOG_ERROR(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_WARN:
        LOG_WARN(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_INFO:
        LOG_INFO(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_V:
        LOG_DEBUG(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    case MPV_LOG_LEVEL_DEBUG:
        LOG_TRACE(LOG_MPV, "{}: {}", msg->prefix, msg->text); break;
    default: // unexpected (e.g. TRACE — we cap subscription at debug) or new mpv level
        LOG_WARN(LOG_MPV, "[unhandled mpv level {}] {}: {}",
                 (int)msg->log_level, msg->prefix, msg->text); break;
    }
}

static void mpv_digest_thread() {
    while (!g_shutting_down.load(std::memory_order_relaxed)) {
        mpv_event* ev = g_mpv.WaitEvent(-1);
        if (ev->event_id == MPV_EVENT_NONE) continue;

        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            log_mpv_message(static_cast<mpv_event_log_message*>(ev->data));
            continue;
        }

        if (ev->event_id == MPV_EVENT_SHUTDOWN) {
            LOG_INFO(LOG_MAIN, "MPV_EVENT_SHUTDOWN received");
            MpvEvent se{MpvEventType::SHUTDOWN};
            publish(se);
            initiate_shutdown();
            return;
        }

        if (ev->event_id == MPV_EVENT_FILE_LOADED) {
            MpvEvent fe{MpvEventType::FILE_LOADED};
            publish(fe);
            continue;
        }

        if (ev->event_id == MPV_EVENT_END_FILE) {
            auto* d = static_cast<mpv_event_end_file*>(ev->data);
            MpvEvent fe{};
            if (d->reason == MPV_END_FILE_REASON_EOF)
                fe.type = MpvEventType::END_FILE_EOF;
            else if (d->reason == MPV_END_FILE_REASON_STOP)
                fe.type = MpvEventType::END_FILE_CANCEL;
            else {
                fe.type = MpvEventType::END_FILE_ERROR;
                // mpv_error_string returns a pointer to a static, never-freed
                // string — safe to carry across threads via MpvEvent.
                fe.err_msg = mpv_error_string(d->error);
            }
            publish(fe);
            continue;
        }

        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* p = static_cast<mpv_event_property*>(ev->data);
            MpvEvent me = digest_property(ev->reply_userdata, p);
            if (me.type == MpvEventType::NONE) continue;
            if (me.type == MpvEventType::OSD_DIMS) {
                if (me.lw <= 0 || me.lh <= 0) continue;
            }
            if (me.type == MpvEventType::FULLSCREEN) {
                g_platform.set_fullscreen(me.flag);
            }
            publish(me);
        }
    }
}


// Shutdown order (reverse of declaration):
//   browsers → CefShutdown → idle_inhibit clear → platform.cleanup
// then main runs mpv terminate + post_window_cleanup.
static int run_with_cef(int mw, int mh,
                        const cli::Args& args,
                        LogLevel log_level) {
    std::string ozone_platform = args.ozone_platform;
#if !defined(_WIN32) && !defined(__APPLE__)
    if (ozone_platform.empty())
        ozone_platform = g_platform.display == DisplayBackend::Wayland ? "wayland" : "x11";
#endif
    g_platform.cef_ozone_platform = ozone_platform;
    PlatformScope platform_scope(g_platform, g_mpv.Get());
    if (!platform_scope.ok()) {
        LOG_ERROR(LOG_MAIN, "Platform init failed");
        return 1;
    }
    LOG_INFO(LOG_MAIN, "Platform init ok");

    IdleInhibitGuard idle_inhibit_guard;

    // Apply titlebar color before CefInitialize so the window doesn't sit
    // with the system default palette for the whole CEF init duration.
    if (Settings::instance().titlebarThemeColor())
        g_platform.set_theme_color(kBgColor);

    // Must run after the VO-init wait loop — sync mpv API calls would
    // deadlock against core_thread's DispatchQueue.main.sync on macOS.
    {
        auto caps = mpv_capabilities::Query(g_mpv.Get());
        jellyfin_device_profile::SetCachedJson(jellyfin_device_profile::Build(
            caps, "Jellyfin Desktop", APP_VERSION_FULL,
            Settings::instance().forceTranscoding()));
    }

    bool use_shared_textures = g_platform.shared_texture_supported && !args.disable_gpu_compositing;

    CefRuntime::SetLogSeverity(toCefSeverity(log_level));
    CefRuntime::SetRemoteDebuggingPort(args.remote_debugging_port);
    CefRuntime::SetDisableGpuCompositing(!use_shared_textures);
#ifdef __linux__
    if (!ozone_platform.empty())
        CefRuntime::SetOzonePlatform(ozone_platform);
#endif

    LOG_INFO(LOG_MAIN, "[FLOW] calling CefInitialize...");
    CefRuntimeScope cef_scope;
    if (!cef_scope.ok()) {
        LOG_ERROR(LOG_MAIN, "CefInitialize failed");
        return 1;
    }
    LOG_INFO(LOG_MAIN, "[FLOW] CefInitialize returned ok");

    double display_hidpi_scale = 0.0;
    mpv_get_property(g_mpv.Get(), "display-hidpi-scale",
                     MPV_FORMAT_DOUBLE, &display_hidpi_scale);
    int fs_flag = 0;
    mpv_get_property(g_mpv.Get(), "fullscreen", MPV_FORMAT_FLAG, &fs_flag);
    mpv::seed_display_hz_sync(g_mpv);
    LOG_INFO(LOG_MAIN, "[FLOW] display-hidpi-scale={} fullscreen={} display-hz={}",
             display_hidpi_scale, fs_flag, mpv::display_hz());

    // Scale-correct the window size when live display scale differs from
    // saved. Skip while the compositor has the surface locked
    // (fullscreen/maximized): mpv's wayland set_geometry runtime path
    // unconditionally writes wl->window_size and fires VO_EVENT_RESIZE,
    // which makes osd-dimensions emit the corrected size and CEF resize to
    // it — while the actual surface stays at the locked size. Internal/
    // visual size diverge ("sometimes" depending on whether the compositor
    // re-issues a configure). Defer: the next clean unmaximize/unfullscreen
    // restores to mpv's pre-init geometry value, the user resizes once, and
    // shutdown saves a matching scale so subsequent launches need no
    // correction.
    {
        const auto& saved = Settings::instance().windowGeometry();
        bool locked = fs_flag || mpv::window_maximized();
        // Only correct when we have a real saved scale that differs from
        // live. Fresh-config (saved.scale == 0) was already computed at the
        // live scale by the pre-init probe; re-issuing SetGeometry here
        // takes mpv's runtime geometry path which bypasses configure_bounds.
        if (!locked && display_hidpi_scale > 0.0 && saved.scale > 0.f &&
            std::fabs(display_hidpi_scale - saved.scale) >= 0.01) {
            int new_pw = static_cast<int>(
                std::lround(saved.logical_width  * display_hidpi_scale));
            int new_ph = static_cast<int>(
                std::lround(saved.logical_height * display_hidpi_scale));
            int dummy_x = -1, dummy_y = -1;
            if (g_platform.clamp_window_geometry)
                g_platform.clamp_window_geometry(&new_pw, &new_ph,
                                                 &dummy_x, &dummy_y);
            std::string geom_str = std::to_string(new_pw) + "x"
                                 + std::to_string(new_ph);
            LOG_INFO(LOG_MAIN,
                     "[FLOW] scale {:.3f} -> {:.3f}, resize to {}",
                     saved.scale, display_hidpi_scale, geom_str.c_str());
            g_mpv.SetGeometry(geom_str);
            mw = new_pw;
            mh = new_ph;
        }
        mpv::set_window_pixels(mw, mh);
    }

    float scale = display_hidpi_scale > 0.0
        ? static_cast<float>(display_hidpi_scale)
        : g_platform.get_scale();
    int lw = static_cast<int>(mw / scale);
    int lh = static_cast<int>(mh / scale);

    // Must exist before main browser creation: the pre-loaded page fires
    // its initial theme-color IPC at DOMContentLoaded; onOverlayDismissed
    // needs a color already captured.
    bool titlebar_themed = Settings::instance().titlebarThemeColor();
    ThemeColor theme_color_obj([titlebar_themed](const Color& c) {
        if (titlebar_themed) g_platform.set_theme_color(c);
        g_mpv.SetBackgroundColor(c);
    });
    g_theme_color = &theme_color_obj;

    Browsers browsers(lw, lh, mw, mh, mpv::display_hz(), use_shared_textures);
    g_browsers = &browsers;

    auto main_layer = browsers.create(WebBrowser::injectionProfile());
    auto web_browser_owner = std::make_unique<WebBrowser>(main_layer);
    g_web_browser = web_browser_owner.get();

    std::string server_url = Settings::instance().serverUrl();
    std::string main_url;
    // Eager pre-load: fetch the saved server while the overlay probes in
    // parallel. The overlay fades out on success, revealing the loaded page.
    if (!server_url.empty())
        main_url = server_url;

    LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(main) url={} lw={} lh={} pw={} ph={}",
             main_url.c_str(), lw, lh, mw, mh);
    main_layer->create(main_url);
    LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(main) call returned");

    std::unique_ptr<OverlayBrowser> overlay_browser_owner;
    {
        auto overlay_layer = browsers.create(OverlayBrowser::injectionProfile());
        overlay_layer->setVisible(true);
        overlay_browser_owner = std::make_unique<OverlayBrowser>(
            overlay_layer, *g_web_browser);
        LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(overlay)");
        overlay_layer->create("app://resources/overlay.html");
        LOG_INFO(LOG_MAIN, "[FLOW] CreateBrowser(overlay) call returned");
    }

    // Coordinator + sinks must exist before any thread can post inputs or
    // observe playback state. Sinks register before start() so the worker
    // never delivers to a half-built fanout.
    PlaybackCoordinatorScope coord_scope;
    g_playback_coord = &coord_scope.coord();
    auto browser_sink = std::make_shared<BrowserPlaybackSink>();
    auto idle_inhibit_sink = std::make_shared<IdleInhibitSink>();
    auto theme_color_sink = std::make_shared<ThemeColorSink>();
    auto mpv_action_sink = std::make_shared<MpvActionSink>();
    coord_scope.coord().addSink(browser_sink);
    coord_scope.coord().addSink(idle_inhibit_sink);
    coord_scope.coord().addSink(theme_color_sink);
#if defined(__APPLE__)
    auto media_sink = std::make_shared<MacosSink>();
#elif defined(_WIN32)
    int64_t wid = 0;
    g_mpv.GetPropertyInt("window-id", wid);
    auto media_sink = std::make_shared<WindowsSink>(reinterpret_cast<HWND>(static_cast<intptr_t>(wid)));
#else
    auto media_sink = std::make_shared<MprisSink>();
#endif
    coord_scope.coord().addSink(media_sink);
    media_sink->start();
    coord_scope.coord().addActionSink(mpv_action_sink);
    register_queued_sinks(
        {browser_sink, idle_inhibit_sink, theme_color_sink},
        {mpv_action_sink});

    // Start before waitForLoad so mpv events (OSD_DIMS especially) reach
    // the platform/browsers during the overlay-only startup phase, before
    // the main browser finishes loading.
    LOG_INFO(LOG_MAIN, "[FLOW] starting digest + cef_consumer threads");
    std::thread digest_thread(mpv_digest_thread);
    std::thread cef_thread(cef_consumer_thread);

#ifndef __APPLE__
    g_web_browser->waitForLoad();
#endif
    LOG_INFO(LOG_MAIN, "Main browser loaded");

    LOG_INFO(LOG_MAIN, "[FLOW] Running — about to enter run_main_loop");

#ifdef __APPLE__
    // Block on the NSApplication run loop until initiate_shutdown calls
    // wake_main_loop. Services NSEvents and GCD main-queue blocks (mpv VO
    // DispatchQueue.main.sync, CEF App::OnScheduleMessagePumpWork).
    g_platform.run_main_loop();
    LOG_INFO(LOG_MAIN, "[FLOW] run_main_loop returned — entering post-run drain");

    // CEF may still have browser-close work in flight after the main loop
    // breaks. Spin the runloop event-driven until all browsers report closed.
    while (g_browsers && !g_browsers->allClosed()) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 60.0, true);
    }

#else
    if (g_browsers) g_browsers->waitAllClosed();
#endif

    g_theme_color = nullptr;
    media_sink->stop();

    cef_thread.join();
    g_mpv.Wakeup();
    digest_thread.join();

    // Producers have joined; coordinator drains any in-flight inputs and
    // stops via PlaybackCoordinatorScope dtor at end of scope. Clear the
    // global pointer first so any late readers see "no coordinator".
    g_playback_coord = nullptr;

    // Save window geometry while mpv is still alive.
    {
        bool fs  = mpv::fullscreen();
        bool max = mpv::window_maximized();

        if (fs) {
            // Don't overwrite the saved windowed size with fullscreen dims;
            // only update the maximized flag for the eventual restore.
            auto geom = Settings::instance().windowGeometry();
            geom.maximized = g_was_maximized_before_fullscreen;
            Settings::instance().setWindowGeometry(geom);
        } else if (max) {
            // Don't overwrite the saved windowed size with monitor dims;
            // on next launch the window opens maximized and unmaximize
            // restores the preserved size.
            auto geom = Settings::instance().windowGeometry();
            geom.maximized = true;
            Settings::instance().setWindowGeometry(geom);
        } else {
            // Capture {pixel, logical, scale} so the next launch can
            // restore losslessly on the same display, or rescale on a
            // display with different DPI. Prefer window_pw/ph (set at boot)
            // over osd_pw/ph which may lag a resize we just issued.
            int pw = mpv::window_pw();
            int ph = mpv::window_ph();
            if (pw <= 0 || ph <= 0) {
                pw = mpv::osd_pw();
                ph = mpv::osd_ph();
            }
            if (pw > 0 && ph > 0) {
                Settings::WindowGeometry geom;
                geom.width = pw;
                geom.height = ph;

                float win_scale = g_platform.get_scale ? g_platform.get_scale() : 1.0f;
                if (win_scale <= 0.f) win_scale = 1.0f;
                geom.scale = win_scale;
                geom.logical_width  = static_cast<int>(std::lround(pw / win_scale));
                geom.logical_height = static_cast<int>(std::lround(ph / win_scale));

                geom.maximized = false;
                int wx, wy;
                if (g_platform.query_window_position &&
                    g_platform.query_window_position(&wx, &wy)) {
                    geom.x = wx;
                    geom.y = wy;
                }
                Settings::instance().setWindowGeometry(geom);
            }
        }
        Settings::instance().save();
    }

    // Business owners released first — their dtors call g_browsers->remove,
    // freeing the platform surfaces and clearing the active pointer. About
    // is a self-managed singleton: its BeforeCloseCallback already deleted
    // it during the close drain above. Any straggler surface gets freed by
    // Browsers' dtor when `browsers` goes out of scope.
    g_web_browser = nullptr;
    web_browser_owner.reset();
    overlay_browser_owner.reset();
    g_browsers = nullptr;
    // `browsers` itself goes out of scope here; any straggler surfaces
    // get freed by its dtor.

    return 0;
}

// =====================================================================
// Main
// =====================================================================

int main(int argc, char* argv[]) {
    // CEF subprocesses (GPU, renderer) re-execute this binary; they must
    // hit CefExecuteProcess immediately, before CLI parsing or anything
    // else touches shared state. Linux platform selection is deferred
    // until after CLI parsing.
#ifdef _WIN32
    g_platform = make_platform(DisplayBackend::Windows);
#elif defined(__APPLE__)
    g_platform = make_platform(DisplayBackend::macOS);
#endif

    if (int rc = CefRuntime::Start(argc, argv); rc >= 0) return rc;

    Settings::instance().load();
    const auto& saved = Settings::instance();
    cli::Args args;
    args.hwdec = !saved.hwdec().empty() ? saved.hwdec() : std::string(kHwdecDefault);
    args.audio_passthrough = saved.audioPassthrough();
    args.audio_exclusive = saved.audioExclusive();
    args.audio_channels = saved.audioChannels();
    args.log_level = saved.logLevel();

    cli::Result cli_result = cli::parse(argc, argv, args);
    switch (cli_result.kind) {
    case cli::Result::Kind::Help:
        cli::print_help();
        return 0;
    case cli::Result::Kind::Version:
        cli::print_version();
        return 0;
    case cli::Result::Kind::Error:
        fprintf(stderr, "Error: unknown argument '%s'\n", cli_result.unknown_arg.c_str());
        return 1;
    case cli::Result::Kind::Continue:
        break;
    }

    if (!isValidHwdec(args.hwdec)) args.hwdec = kHwdecDefault;

    // --log-file overrides default; empty argument disables file logging entirely.
    // Default to a platform log file on macOS/Windows (GUI apps have no
    // user-visible stderr there). On Linux, stderr/journalctl is the norm,
    // so file logging is opt-in via --log-file.
    std::string log_path;
    if (args.log_file) {
        log_path = *args.log_file;
    } else {
#if !defined(__linux__)
        log_path = paths::getLogPath();
#endif
    }
    LogLevel log_level = LogLevel::Default;
    if (!args.log_level.empty()) {
        log_level = parseLogLevel(args.log_level.c_str());
        if (log_level == LogLevel::Default) {
            fprintf(stderr, "Invalid log level: '%s' (expected trace|debug|info|warn|error)\n",
                    args.log_level.c_str());
            return 1;
        }
    }
    LoggingScope logging(log_path.c_str(), log_level);

    LOG_INFO(LOG_MAIN, "jellyfin-desktop " APP_VERSION_FULL);
    LOG_INFO(LOG_MAIN, "CEF {}", CEF_VERSION);
    if (!log_path.empty()) LOG_INFO(LOG_MAIN, "Log file: {}", log_path.c_str());

#if !defined(_WIN32) && !defined(__APPLE__)
    {
        DisplayBackend backend;
        if (args.platform_override == "wayland")
            backend = DisplayBackend::Wayland;
        else if (args.platform_override == "x11")
            backend = DisplayBackend::X11;
        else if (!args.platform_override.empty()) {
            fprintf(stderr, "Unknown platform: %s (expected wayland or x11)\n",
                    args.platform_override.c_str());
            return 1;
        } else {
            backend = (getenv("WAYLAND_DISPLAY") || !getenv("DISPLAY"))
                    ? DisplayBackend::Wayland : DisplayBackend::X11;
        }
#ifndef HAVE_X11
        if (backend == DisplayBackend::X11) {
            fprintf(stderr, "X11 detected but X11 support not compiled in\n");
            return 1;
        }
#endif
        g_platform = make_platform(backend);
    }
    LOG_INFO(LOG_MAIN, "Display backend: {}",
             g_platform.display == DisplayBackend::Wayland ? "wayland" : "x11");
#endif

#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD) -> BOOL {
        initiate_shutdown();
        return TRUE;
    }, TRUE);
#else
    SignalHandlerGuard signal_guard(signal_handler);
#endif

#ifndef __APPLE__
    if (trySignalExisting()) {
        LOG_INFO(LOG_MAIN, "Signaled existing instance, exiting");
        return 0;
    }
    startListener([](const std::string&) {
        // TODO: raise window via xdg-activation
    });
    // Joins the listener thread on any exit path (a joinable std::thread
    // calls std::terminate from its destructor).
    struct ListenerGuard { ~ListenerGuard() { stopListener(); } } listener_guard;
#endif

    std::string mpv_home = paths::getMpvHome();
#ifdef _WIN32
    SetEnvironmentVariableA("MPV_HOME", mpv_home.c_str());
#else
    setenv("MPV_HOME", mpv_home.c_str(), 1);
#endif

    g_mpv = MpvHandle::Create(g_platform.display);
    if (!g_mpv.IsValid()) { LOG_ERROR(LOG_MAIN, "mpv_create failed"); return 1; }

    // libmpv defaults config=no (opposite of the mpv CLI); enable it so
    // users' $MPV_HOME/mpv.conf is loaded.
    g_mpv.SetOptionString("config", "yes");

    // We only ever feed mpv direct media URLs from the Jellyfin server;
    // the youtube-dl/yt-dlp hook would just add startup latency and a
    // failure mode for nothing.
    g_mpv.SetOptionString("ytdl", "no");

    g_mpv.SetOptionString("user-agent", APP_USER_AGENT);

    // 'auto' skips bounds when --geometry is set; force 'yes'.
    g_mpv.SetOptionString("wayland-configure-bounds", "yes");

    g_mpv.SetHwdec(args.hwdec);

    // Restore saved window geometry. mpv's --geometry is always physical
    // pixels (m_geometry_apply at third_party/mpv/options/m_option.c:2296
    // assigns gm->w/h to widw/widh without applying dpi_scale), so we pass
    // physical pixels here. If the live display scale differs from what
    // these pixels were computed against, the post-CEF-init resize block
    // below corrects the window size once display-hidpi-scale is known.
    {
        using WG = Settings::WindowGeometry;
        auto saved_geom = Settings::instance().windowGeometry();

        int x = saved_geom.x, y = saved_geom.y;
        float scale = g_platform.get_display_scale(x, y);
        int w, h;
        if (saved_geom.logical_width > 0 && saved_geom.logical_height > 0) {
            w = static_cast<int>(std::lround(saved_geom.logical_width  * scale));
            h = static_cast<int>(std::lround(saved_geom.logical_height * scale));
        } else if (saved_geom.width > 0 && saved_geom.height > 0) {
            w = saved_geom.width;
            h = saved_geom.height;
        } else {
            w = static_cast<int>(std::lround(WG::kDefaultLogicalWidth  * scale));
            h = static_cast<int>(std::lround(WG::kDefaultLogicalHeight * scale));
        }
        LOG_DEBUG(LOG_MAIN, "initial scale: {} -> {}x{}", scale, w, h);

        if (g_platform.clamp_window_geometry)
            g_platform.clamp_window_geometry(&w, &h, &x, &y);
        std::string geom_str = std::to_string(w) + "x" + std::to_string(h);
        if (x >= 0 && y >= 0) {
            geom_str += "+" + std::to_string(x) + "+" + std::to_string(y);
            g_mpv.SetOptionString("force-window-position", "yes");
        }
        g_mpv.SetOptionString("geometry", geom_str);
        if (saved_geom.maximized)
            g_mpv.SetOptionString("window-maximized", "yes");
    }

    if (!args.audio_passthrough.empty()) {
        // Normalize: dts-hd subsumes dts
        if (args.audio_passthrough.find("dts-hd") != std::string::npos) {
            std::string filtered;
            size_t pos = 0;
            while (pos < args.audio_passthrough.size()) {
                size_t comma = args.audio_passthrough.find(',', pos);
                if (comma == std::string::npos) comma = args.audio_passthrough.size();
                std::string codec = args.audio_passthrough.substr(pos, comma - pos);
                if (codec != "dts") {
                    if (!filtered.empty()) filtered += ',';
                    filtered += codec;
                }
                pos = comma + 1;
            }
            args.audio_passthrough = filtered;
        }
        g_mpv.SetAudioSpdif(args.audio_passthrough);
    }
    if (args.audio_exclusive)
        g_mpv.SetAudioExclusive(true);
    if (!args.audio_channels.empty())
        g_mpv.SetAudioChannels(args.audio_channels);

    // Register property observations before mpv_initialize. On macOS,
    // core_thread races to DispatchQueue.main.sync immediately after init
    // returns — main must enter the GCD pump loop without delay.
    g_mpv.SetWakeupCallback([](void*) {}, nullptr);
    observe_properties(g_mpv);

    int init_err = g_mpv.Initialize();
    if (init_err < 0) {
        LOG_ERROR(LOG_MAIN, "mpv_initialize failed: {}", init_err);
        return 1;
    }
    g_mpv.SetLogLevel(log_level);

    // Capture user's mpv.conf bg, then force startup color. Safe here:
    // force-window=yes (not "immediate") defers VO creation, so the user's
    // color never flashes before we override.
    g_video_bg = g_mpv.GetBackgroundColor();
    LOG_INFO(LOG_MAIN, "video bg captured: {}", g_video_bg.hex);
    g_mpv.SetBackgroundColor(kBgColor);

    for (const char* prop : {"mpv-version", "ffmpeg-version"})
        LOG_INFO(LOG_MAIN, "{} {}", prop, g_mpv.GetPropertyString(prop));

    // input-default-bindings=no removes all builtin bindings including
    // CLOSE_WIN → quit.  Re-bind it so the WM close button works.
    {
        const char* cmd[] = {"keybind", "CLOSE_WIN", "quit", nullptr};
        mpv_command(g_mpv.Get(), cmd);
    }

    // Wait for the VO window. Reads osd-dimensions from the event payload
    // (no sync mpv_get_property call) so it stays safe against a
    // DispatchQueue.main.sync deadlock against core_thread on macOS.
    LOG_INFO(LOG_MAIN, "Waiting for mpv window...");
    int64_t mw = 0, mh = 0;
    // First OSD_DIMS event reflects the pre-configure geometry hint, not the
    // post-configure surface size. When maximized startup is requested, also
    // wait for the window-maximized property to flip true (proves mpv has
    // processed the compositor's maximize configure) and take the OSD_DIMS
    // that follows.
    bool need_max = Settings::instance().windowGeometry().maximized;
    auto consume = [&](mpv_event* ev) -> bool {
        if (ev->event_id != MPV_EVENT_PROPERTY_CHANGE) return false;
        MpvEvent me = digest_property(
            ev->reply_userdata, static_cast<mpv_event_property*>(ev->data));
        if (ev->reply_userdata == MPV_OBSERVE_WINDOW_MAX &&
            mpv::window_maximized())
            need_max = false;
        if (me.type == MpvEventType::OSD_DIMS && me.pw > 0 && me.ph > 0) {
            mw = me.pw;
            mh = me.ph;
        }
        return mw > 0 && !need_max;
    };

#ifdef __APPLE__
    while (true) {
        g_platform.pump();
        mpv_event* ev = g_mpv.WaitEvent(0);
        if (ev->event_id == MPV_EVENT_NONE) { usleep(10000); continue; }
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            log_mpv_message(static_cast<mpv_event_log_message*>(ev->data));
            continue;
        }
        if (ev->event_id == MPV_EVENT_SHUTDOWN || ev->event_id == MPV_EVENT_END_FILE) {
            return 0;
        }
        if (consume(ev)) break;
    }
#else
    while (true) {
        mpv_event* ev = g_mpv.WaitEvent(1.0);
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            log_mpv_message(static_cast<mpv_event_log_message*>(ev->data));
            continue;
        }
        if (ev->event_id == MPV_EVENT_SHUTDOWN) return 0;
        if (ev->event_id == MPV_EVENT_END_FILE) return 0;
        if (consume(ev)) break;
    }
#endif

    int rc = run_with_cef(static_cast<int>(mw), static_cast<int>(mh),
                          args, log_level);
    if (rc != 0) return rc;

#ifdef __APPLE__
    // mpv's VO uninit (mac_common.swift:84) does DispatchQueue.main.sync
    // to close its window — calling TerminateDestroy from the main thread
    // would deadlock. Run it on a side thread and pump the runloop here
    // (same pattern as Chromium's MessagePumpCFRunLoop::DoRun).
    std::atomic<bool> mpv_done{false};
    std::thread mpv_teardown([&mpv_done]{
        // CefInitialize reset SIGALRM to SIG_DFL (content_main.cc:108);
        // mpv's PreciseTimer.terminate() sends pthread_kill(SIGALRM), so
        // restore a no-op handler before tearing down the timer.
        signal(SIGALRM, [](int){});
        g_mpv.TerminateDestroy();
        mpv_done.store(true, std::memory_order_release);
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
    while (!mpv_done.load(std::memory_order_acquire))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,
                           std::numeric_limits<CFTimeInterval>::max(), true);
    mpv_teardown.join();
#else
    g_mpv.TerminateDestroy();
#endif

    if (g_platform.post_window_cleanup)
        g_platform.post_window_cleanup();

    return 0;
}
